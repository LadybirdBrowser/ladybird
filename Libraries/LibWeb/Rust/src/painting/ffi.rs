/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::used_values::FfiCssPixelSize;
use crate::layout::{grid_formatting_context, svg_formatting_context, used_values};
use crate::painting::display_list::commands::FrameNodeIndex;
use crate::painting::display_list::commands::SpatialNodeIndex;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::host::FfiRecordedDisplayList;
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::{PaintableRowsRead, with_inline_pieces};
use crate::painting::rect_to_viewport_transform::RectToViewportTransform;
use crate::painting::scroll_chain::ViewportWheelOverflow;
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FilterOperationType {
    Arithmetic,
    Compose,
    Blend,
    Flood,
    DisplacementMap,
    DropShadow,
    Blur,
    ColorFilter,
    ColorMatrix,
    ColorTable,
    Saturate,
    HueRotate,
    Image,
    Merge,
    Offset,
    Erode,
    Dilate,
    Turbulence,
    ColorSpaceConversion,
}

#[derive(Clone, Copy, Debug, Default)]
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
    rows.paintable_data_mut(slot).set_flag(flag, enlarged);
}

/// Decide if force-dark should invert an image: the caller owns the sampling, this owns the policy. Returns false
/// for an empty or null buffer, which leaves the image untouched.
///
/// # Safety
/// `samples` must point to `count` packed colors, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_force_dark_should_filter_image(
    samples: *const u32,
    count: usize,
    transparency_ratio: f32,
) -> bool {
    if samples.is_null() || count == 0 {
        return false;
    }
    // Color is repr(transparent) over u32, so the buffer needs no copy to be read as colors.
    let colors = unsafe { std::slice::from_raw_parts(samples.cast::<libgfx_rust::Color>(), count) };
    let features = crate::painting::force_dark::features_from_samples(colors, transparency_ratio);
    crate::painting::force_dark::should_filter(&features)
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
pub unsafe extern "C" fn layout_arena_schedule_scrollable_overflow_recalculation(arena: *mut c_void, node: NodeSlotId) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.schedule_scrollable_overflow_recalculation(node);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_needs_full_scrollable_overflow_recalculation(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.needs_full_scrollable_overflow_recalculation.get()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_needs_full_scrollable_overflow_recalculation(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.needs_full_scrollable_overflow_recalculation.set(true);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
/// The drained slot ids may name freed slots; the caller resolves liveness before
/// dereferencing anything.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_scrollable_overflow_recalculation_state(
    arena: *mut c_void,
    context: *mut c_void,
    push_box: unsafe extern "C" fn(*mut c_void, NodeSlotId),
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let (boxes, needs_full_recalculation) = arena.take_scrollable_overflow_recalculation_state();
    for slot in boxes {
        // SAFETY: The C++ callback appends the slot id to a caller-owned collection.
        unsafe { push_box(context, slot) };
    }
    needs_full_recalculation
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
        arena.invalidate_paint_cache(ancestor);
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_supports_svg_masking(arena: *mut c_void, slot: NodeSlotId) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_rows().paintable_row_is_populated(slot) {
        return false;
    }
    arena
        .node_kind_if_live(slot)
        .is_some_and(crate::painting::node_painting::supports_svg_masking)
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
pub unsafe extern "C" fn layout_arena_paintable_overflow_is_valid(arena: *mut c_void, slot: NodeSlotId) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let rows = arena.paintable_rows();
    rows.paintable_row_is_populated(slot)
        && (rows.paintable_data(slot).overflow_measured_this_commit
            || arena.paintable_side_data(slot).overflow_valid_across_recommits.get())
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_cleared_from_node(arena: *mut c_void, layout_node: NodeSlotId) {
    let reset = {
        let arena = unsafe { arena_from_handle(arena) };
        arena.invalidate_paint_cache(layout_node);
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

pub(crate) unsafe fn ffi_slice<'a, T>(data: *const T, length: usize) -> &'a [T] {
    assert!(!data.is_null() || length == 0);
    if length == 0 {
        return &[];
    }
    // SAFETY: The caller guarantees `data` points at `length` valid values for
    // the duration of the borrow.
    unsafe { std::slice::from_raw_parts(data, length) }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
pub(crate) unsafe fn tree_from_handle<'a>(
    tree: *const c_void,
) -> &'a crate::painting::visual_context::VisualContextTree {
    // SAFETY: The caller guarantees `tree` is a live retained handle.
    unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() }
}

/// # Safety
///
/// Both trees must be live retained tree handles and every pointer must address the stated number of
/// bytes or points for the call. Writes the damage rect through `out_damage_rect` and returns whether
/// the damage is bounded; unbounded damage means the whole viewport must repaint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_compute_damage(
    old_command_bytes: *const u8,
    old_command_bytes_length: usize,
    old_tree: *const c_void,
    old_scroll_offsets: *const libgfx_rust::FloatPoint,
    old_scroll_offsets_len: usize,
    new_command_bytes: *const u8,
    new_command_bytes_length: usize,
    new_tree: *const c_void,
    new_scroll_offsets: *const libgfx_rust::FloatPoint,
    new_scroll_offsets_len: usize,
    viewport_rect: libgfx_rust::IntRect,
    out_damage_rect: *mut libgfx_rust::IntRect,
) -> bool {
    let (old_tree, new_tree) = unsafe { (tree_from_handle(old_tree), tree_from_handle(new_tree)) };
    // SAFETY: The caller guarantees the slices address the stated number of values.
    let (old_command_bytes, old_scroll_offsets, new_command_bytes, new_scroll_offsets) = unsafe {
        (
            ffi_slice(old_command_bytes, old_command_bytes_length),
            ffi_slice(old_scroll_offsets, old_scroll_offsets_len),
            ffi_slice(new_command_bytes, new_command_bytes_length),
            ffi_slice(new_scroll_offsets, new_scroll_offsets_len),
        )
    };
    let damage = crate::painting::display_list::damage::compute_display_list_damage(
        old_command_bytes,
        old_tree,
        old_scroll_offsets,
        new_command_bytes,
        new_tree,
        new_scroll_offsets,
        viewport_rect,
    );
    match damage {
        Some(damage_rect) => {
            // SAFETY: The caller guarantees `out_damage_rect` is writable.
            unsafe { *out_damage_rect = damage_rect };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle, `command_runs` must address `command_run_count`
/// runs and `scroll_offsets` `scroll_offsets_len` points for the call, and `callbacks` must be
/// live. The painter callbacks run synchronously and may re-enter this function for a nested
/// display list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_replay(
    tree: *const c_void,
    command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    command_run_count: usize,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    callbacks: *const crate::painting::host::FfiDisplayListReplayCallbacks,
) {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the slices address the stated number of values.
    let (command_runs, scroll_offsets) = unsafe {
        (
            ffi_slice(command_runs, command_run_count),
            ffi_slice(scroll_offsets, scroll_offsets_len),
        )
    };
    // SAFETY: The caller guarantees `callbacks` is live for the call.
    let mut painter = unsafe { *callbacks };
    crate::painting::display_list::replay::replay_display_list(tree, command_runs, scroll_offsets, &mut painter);
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

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_clear_overflow_data(arena: *mut c_void, slot: NodeSlotId) {
    let arena = unsafe { arena_from_handle_mut(arena) };
    let mut paintable_rows = arena.paintable_rows_mut();
    if paintable_rows.paintable_row_is_populated(slot) {
        paintable_rows.paintable_data_mut(slot).overflow_measured_this_commit = false;
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_clear_cached_overflow_data(arena: *mut c_void, slot: NodeSlotId) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.paintable_rows().clear_cached_overflow_data(slot);
}

#[repr(C)]
pub struct FfiPhysicalOverflowDirections {
    pub horizontal_axis_is_positive: bool,
    pub vertical_axis_is_positive: bool,
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_measure_scrollable_overflow(
    arena: *mut c_void,
    box_paintable: NodeSlotId,
    visual_context_callbacks: crate::painting::host::FfiVisualContextHostCallbacks,
    overflow_callbacks: crate::painting::host::FfiScrollableOverflowHostCallbacks,
) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe {
        measure_scrollable_overflow_for_slot(arena, box_paintable, &visual_context_callbacks, &overflow_callbacks);
    };
}

/// Whether a box is measured eagerly after a full commit rather than only when an ancestor's
/// measurement reaches it: a scroll container, or a box whose element or pseudo-element stores a
/// scroll offset that the new overflow may have to clamp. The offset lives on the DOM side, which
/// sets `NodeFlag::HasScrollOffset` whenever it stores one and whenever a box becomes an element's
/// or pseudo-element's box, so the answer is one style query and one flag read.
fn box_holds_scroll_state(arena: &LayoutNodeArena, slot: NodeSlotId) -> bool {
    crate::painting::style_queries::is_scroll_container(arena, slot)
        || arena.node_flags_if_live(slot) & crate::layout::node_data::NodeFlag::HasScrollOffset as u32 != 0
}

/// # Safety
///
/// `arena_handle` must be a live handle from `layout_arena_create`, used on the document
/// thread, with no outstanding borrows of the arena.
unsafe fn measure_scrollable_overflow_for_slot(
    arena_handle: *mut c_void,
    box_paintable: NodeSlotId,
    visual_context_callbacks: &crate::painting::host::FfiVisualContextHostCallbacks,
    overflow_callbacks: &crate::painting::host::FfiScrollableOverflowHostCallbacks,
) {
    let assignments = {
        // SAFETY: Guaranteed by the caller.
        let arena = unsafe { arena_from_handle(arena_handle) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(box_paintable) {
            return;
        }
        let paint_state = arena.paint_state().borrow();
        crate::painting::scrollable_overflow::measure_scrollable_overflow(
            &paintable_rows,
            &paint_state.scrollable_overflow_contained_boxes,
            visual_context_callbacks,
            overflow_callbacks,
            box_paintable,
        )
    };
    // SAFETY: The shared borrow above ended with its scope.
    let arena = unsafe { arena_from_handle_mut(arena_handle) };
    let mut paintable_rows = arena.paintable_rows_mut();
    for assignment in assignments {
        assignment.apply(&mut paintable_rows);
    }
}

#[repr(C)]
pub struct FfiScrollableOverflowUpdateOutcome {
    pub performed_recalculation: bool,
    pub any_overflow_changed: bool,
    pub any_has_scrollable_overflow_flipped: bool,
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// callbacks receive live layout node shells and must not re-enter the arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_scrollable_overflow(
    arena: *mut c_void,
    viewport: NodeSlotId,
    handled_by_full_layout_commit: bool,
    visual_context_callbacks: crate::painting::host::FfiVisualContextHostCallbacks,
    overflow_callbacks: crate::painting::host::FfiScrollableOverflowHostCallbacks,
    scroll_offset_context: *mut c_void,
    clamp_scroll_offset_if_nonzero: unsafe extern "C" fn(*mut c_void, *mut c_void),
) -> FfiScrollableOverflowUpdateOutcome {
    let no_recalculation = FfiScrollableOverflowUpdateOutcome {
        performed_recalculation: false,
        any_overflow_changed: false,
        any_has_scrollable_overflow_flipped: false,
    };
    let (pending_boxes, needs_full_recalculation) = {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call; every
        // shared borrow below ends before the next exclusive re-derive.
        let arena = unsafe { arena_from_handle(arena) };
        arena.take_scrollable_overflow_recalculation_state()
    };
    if pending_boxes.is_empty() && !needs_full_recalculation {
        return no_recalculation;
    }

    let row_is_populated = |slot: NodeSlotId| {
        // SAFETY: As above.
        unsafe { arena_from_handle(arena) }
            .paintable_rows()
            .paintable_row_is_populated(slot)
    };
    if !row_is_populated(viewport) {
        return no_recalculation;
    }

    let measure_and_clamp = |slot: NodeSlotId| {
        // SAFETY: As above; the callback receives a live shell and does not re-enter.
        unsafe {
            measure_scrollable_overflow_for_slot(arena, slot, &visual_context_callbacks, &overflow_callbacks);
            let shell = arena_from_handle(arena).node_shell(slot);
            clamp_scroll_offset_if_nonzero(scroll_offset_context, shell);
        }
    };

    let performed = FfiScrollableOverflowUpdateOutcome {
        performed_recalculation: true,
        any_overflow_changed: false,
        any_has_scrollable_overflow_flipped: false,
    };
    if handled_by_full_layout_commit {
        assert!(needs_full_recalculation);
        // A full-root commit reset every surviving row, including its overflow data and
        // paint cache. There is therefore no old overflow to preserve or diff. Ordinary
        // boxes are measured recursively when their overflow contributes to one of these
        // roots, so they do not need separate eager measurement: only the viewport, scroll
        // containers, and boxes holding a scroll offset are measured eagerly.
        let mut eager_measurement_roots = Vec::new();
        {
            // SAFETY: As above.
            let arena = unsafe { arena_from_handle(arena) };
            arena.for_each_node_in_layout_subtree_in_pre_order(viewport, |slot| {
                if !arena.paintable_rows().paintable_row_is_populated(slot) {
                    return;
                }
                if slot == viewport || box_holds_scroll_state(arena, slot) {
                    eager_measurement_roots.push(slot);
                }
            });
        }
        for slot in eager_measurement_roots {
            measure_and_clamp(slot);
        }
        return performed;
    }

    // For every box that will be re-measured, the overflow data it had before, so the diff
    // below can tell what actually changed; an empty entry means the box's committed row was
    // reset by a subtree layout commit and the old data is unknown.
    let mut old_overflow_data_by_box: crate::css::style::fast_hash::FastMap<
        NodeSlotId,
        Option<crate::painting::paintable_data::FfiOverflowData>,
    > = Default::default();
    let overflow_snapshot = |slot: NodeSlotId| {
        // SAFETY: As above; callers established a populated row.
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        let data = paintable_rows.paintable_data(slot);
        data.overflow_measured_this_commit
            .then_some(data.overflow_relative_to_padding_box)
    };
    let mut record_and_clear_overflow_data = |slot: NodeSlotId| {
        if !row_is_populated(slot) {
            return true;
        }
        if old_overflow_data_by_box.contains_key(&slot) {
            return false;
        }
        old_overflow_data_by_box.insert(slot, overflow_snapshot(slot));
        // SAFETY: As above; the row was just established as populated.
        unsafe { arena_from_handle_mut(arena) }
            .paintable_rows_mut()
            .paintable_data_mut(slot)
            .overflow_measured_this_commit = false;
        true
    };
    let collect_box_subtree_slots = |root: NodeSlotId| {
        let mut slots = Vec::new();
        // SAFETY: As above; the traversal only reads tree links and kinds.
        let arena = unsafe { arena_from_handle(arena) };
        arena.for_each_node_in_layout_subtree_in_pre_order(root, |slot| {
            if crate::layout::node_facts::kind_is_box(
                arena
                    .node_kind_if_live(slot)
                    .unwrap_or(crate::layout::node_data::NodeKind::Unset),
            ) {
                slots.push(slot);
            }
        });
        slots
    };

    if needs_full_recalculation {
        for slot in collect_box_subtree_slots(viewport) {
            record_and_clear_overflow_data(slot);
        }
        {
            // SAFETY: As above.
            let arena = unsafe { arena_from_handle(arena) };
            let paintable_rows = arena.paintable_rows();
            let mut paint_state = arena.paint_state().borrow_mut();
            crate::painting::scrollable_overflow::refill_contained_boxes_index(
                &paintable_rows,
                viewport,
                &mut paint_state.scrollable_overflow_contained_boxes,
            );
        }
    } else {
        for &pending_slot in &pending_boxes {
            // SAFETY: As above.
            let pending_is_box = row_is_populated(pending_slot)
                && !unsafe { arena_from_handle(arena) }
                    .shell_if_live(pending_slot)
                    .is_null()
                && crate::layout::node_facts::kind_is_box(
                    unsafe { arena_from_handle(arena) }
                        .node_kind_if_live(pending_slot)
                        .unwrap_or(crate::layout::node_data::NodeKind::Unset),
                );
            if !pending_is_box {
                continue;
            }
            let was_reset_by_subtree_layout_commit = overflow_snapshot(pending_slot).is_none();
            if was_reset_by_subtree_layout_commit {
                for slot in collect_box_subtree_slots(pending_slot) {
                    record_and_clear_overflow_data(slot);
                }
            }
            // SAFETY: As above; containing-block links only name live slots.
            let mut containing_block = unsafe { arena_from_handle(arena) }.node_containing_block_if_live(pending_slot);
            while let Some(block) = containing_block {
                if row_is_populated(block) {
                    // SAFETY: As above.
                    unsafe { arena_from_handle(arena) }
                        .paintable_rows()
                        .clear_cached_overflow_data(block);
                }
                if !record_and_clear_overflow_data(block) {
                    break;
                }
                // SAFETY: As above.
                containing_block = unsafe { arena_from_handle(arena) }.node_containing_block_if_live(block);
            }
        }
    }

    if old_overflow_data_by_box.is_empty() {
        return performed;
    }

    for (&slot, old_overflow_data) in &old_overflow_data_by_box {
        if !row_is_populated(slot) {
            continue;
        }
        // Boxes reset by a subtree commit have no previous overflow data. They will be
        // measured recursively if an ancestor reaches them. Measuring each one here would
        // repeatedly walk the same containing-block chains after a small subtree update.
        if old_overflow_data.is_none() && slot != viewport {
            // SAFETY: As above.
            let arena = unsafe { arena_from_handle(arena) };
            if !box_holds_scroll_state(arena, slot) {
                continue;
            }
        }
        measure_and_clamp(slot);
    }

    let mut any_overflow_changed = false;
    let mut any_has_scrollable_overflow_flipped = false;
    for (&slot, old_overflow_data) in &old_overflow_data_by_box {
        if !row_is_populated(slot) {
            continue;
        }
        let Some(new_overflow_data) = overflow_snapshot(slot) else {
            continue;
        };
        // A box with no prior overflow data was just created or reset by the layout commit,
        // so its paint cache is already clean.
        let Some(old_overflow_data) = old_overflow_data else {
            continue;
        };
        // Boxes that merely moved do not register here; everything derived below is
        // translation-invariant, and movement-driven repaint belongs to the layout commit.
        let rect_changed = old_overflow_data.rect != new_overflow_data.rect;
        let has_scrollable_overflow_flipped =
            old_overflow_data.has_scrollable_overflow != new_overflow_data.has_scrollable_overflow;
        if !rect_changed && !has_scrollable_overflow_flipped {
            continue;
        }
        // Cached paint commands and hit-test items capture scrollbar geometry and
        // per-direction scrollability derived from the overflow rect, so they cannot be
        // reused once it changes. This must also run for the after-layout-commit path: a
        // subtree relayout re-measures a surviving ancestor's overflow without resetting
        // the ancestor's committed row.
        // SAFETY: As above.
        unsafe { arena_from_handle(arena) }.invalidate_paint_cache(slot);
        any_overflow_changed = true;
        any_has_scrollable_overflow_flipped |= has_scrollable_overflow_flipped;
    }

    FfiScrollableOverflowUpdateOutcome {
        performed_recalculation: true,
        any_overflow_changed,
        any_has_scrollable_overflow_flipped,
    }
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
pub unsafe extern "C" fn layout_arena_paintable_offset(arena: *mut c_void, slot: NodeSlotId) -> FfiCssPixelPoint {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return FfiCssPixelPoint::default();
    }
    crate::painting::paintable_geometry::committed_offset(&paintable_rows, slot)
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_svg_viewport_user_rect(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> used_values::OptionalCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return None.into();
    }
    let paintable_rows = arena.paintable_rows();
    crate::painting::svg_viewport::nearest_svg_viewport_user_rect(&paintable_rows, slot)
        .map(|rect| used_values::FfiCssPixelRect {
            x: crate::css::css_pixels::CssPixels::nearest_value_for(rect.x as f64),
            y: crate::css::css_pixels::CssPixels::nearest_value_for(rect.y as f64),
            width: crate::css::css_pixels::CssPixels::nearest_value_for(rect.width as f64),
            height: crate::css::css_pixels::CssPixels::nearest_value_for(rect.height as f64),
        })
        .into()
}

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
pub unsafe extern "C" fn layout_arena_rebuild_scrollable_overflow_contained_boxes(
    arena: *mut c_void,
    root: NodeSlotId,
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let mut paint_state = arena.paint_state().borrow_mut();
    crate::painting::scrollable_overflow::refill_contained_boxes_index(
        &paintable_rows,
        root,
        &mut paint_state.scrollable_overflow_contained_boxes,
    );
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_scrollable_overflow_contained_boxes(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena
        .paint_state()
        .borrow_mut()
        .scrollable_overflow_contained_boxes
        .clear();
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
/// `tree` a live retained tree handle. `append` is called synchronously with every node the
/// paintable owns that an animation of the given kind can target.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_visual_animation_target_indices(
    arena: *mut c_void,
    slot: NodeSlotId,
    tree: *const c_void,
    target_kind: crate::painting::host::FfiVisualAnimationTargetKind,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, u32),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let tree = unsafe { tree_from_handle(tree) };
    arena.with_paintable_visual_context_node_handles(slot, |handles| {
        let owned_indices: Vec<u32> = match target_kind {
            crate::painting::host::FfiVisualAnimationTargetKind::Opacity
            | crate::painting::host::FfiVisualAnimationTargetKind::BackgroundColor
            | crate::painting::host::FfiVisualAnimationTargetKind::Filter => {
                handles.frame_handles().map(|index| index.0).collect()
            }
            crate::painting::host::FfiVisualAnimationTargetKind::Transform => {
                handles.spatial.iter().map(|index| index.0).collect()
            }
        };
        for index in owned_indices {
            if tree.visual_animation_target_is_valid(target_kind, index) {
                // SAFETY: the host appends into its own storage synchronously.
                unsafe { append(context, index) };
            }
        }
    });
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
        FfiVisualContextBoxNodeList::FrameNodes => handles.frame_handles().count(),
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
            FfiVisualContextBoxNodeList::FrameNodes => handles.frame_handles().map(|index| index.0).collect(),
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
    root_background_source: crate::painting::host::FfiRootBackgroundSource,
    state: &mut crate::painting::visual_context::VisualContextState,
) -> crate::painting::host::FfiVisualContextUpdateOutcome {
    use crate::painting::visual_context::dirty::VisualContextUpdateScope;
    use crate::painting::visual_context::incremental::{
        IncrementalUpdateResult, debug_assert_every_live_node_is_owned, update_visual_context_tree,
    };
    let fresh_tree = {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        crate::painting::visual_context::build::create_fresh_tree_with_viewport_nodes(
            &paintable_rows,
            viewport,
            &inputs,
        )
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
            root_background_source,
            VisualContextUpdateScope::FreshTree,
            state,
        ) {
            IncrementalUpdateResult::Applied(outcome) => outcome,
            IncrementalUpdateResult::NeedsFullBuild(_) => {
                unreachable!("a fresh tree walk has a tree and a viewport record")
            }
        }
    };
    let arena = unsafe { arena_from_handle_mut(arena) };
    outcome.mask_node_owners_changed = true;
    apply_walk_assignments(arena, viewport, &mut outcome, state);
    arena.rebuild_all_stacking_context_entries_from_records(viewport);
    arena.take_line_roots_needing_fragment_ownership();
    crate::painting::fragment_ownership::assign_fragment_ownership(&arena.paintable_rows(), viewport);
    arena.mark_all_paint_caches_dirty();
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
    use crate::painting::visual_context::dirty::{
        VisualContextBoxDirtyKind, VisualContextGlobalRebuildReason, VisualContextUpdateScope,
    };
    use crate::painting::visual_context::incremental::{
        IncrementalUpdateResult, debug_assert_every_live_node_is_owned, update_visual_context_tree,
    };
    let arena_ref = unsafe { arena_from_handle(arena) };
    if !arena_ref.paintable_row_is_populated(viewport) {
        return crate::painting::host::FfiVisualContextUpdateOutcome::default();
    }
    let inputs = callbacks.tree_inputs();
    let root_background_source = callbacks.root_background_source();
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
    if arena_ref.may_have_default_scroll_shift_anchor() {
        reason = reason.max(VisualContextGlobalRebuildReason::AnchorsRegistered);
    }
    if state.tree.as_deref().is_some_and(|tree| tree.should_compact()) {
        reason = reason.max(VisualContextGlobalRebuildReason::Compaction);
    }
    if let Some(last_source) = state.last_root_background_source
        && (last_source.use_body_background_properties != root_background_source.use_body_background_properties
            || last_source.body_layout_node != root_background_source.body_layout_node)
    {
        for body in [last_source.body_layout_node, root_background_source.body_layout_node] {
            if arena_ref.paintable_row_is_populated(body) {
                let pending_box_limit = arena_ref
                    .paintable_row_count()
                    .max(crate::painting::visual_context::dirty::MINIMUM_PENDING_DIRTY_BOX_LIMIT);
                state.dirty_boxes.note_box(
                    body,
                    VisualContextBoxDirtyKind::StyleStructuralChange,
                    pending_box_limit,
                );
                if let Some(html) = crate::painting::paint_order::paint_parent(&arena_ref.paintable_rows(), body) {
                    state.dirty_boxes.note_box(
                        html,
                        VisualContextBoxDirtyKind::StyleStructuralChange,
                        pending_box_limit,
                    );
                }
            }
        }
    }

    loop {
        let scope = VisualContextUpdateScope::for_reason(reason);
        if scope == VisualContextUpdateScope::FreshTree {
            break;
        }
        let result = {
            let paintable_rows = arena_ref.paintable_rows();
            update_visual_context_tree(
                &paintable_rows,
                &callbacks,
                viewport,
                inputs,
                root_background_source,
                scope,
                &mut state,
            )
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
                state.last_root_background_source = Some(root_background_source);
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
    let outcome =
        fresh_visual_context_tree_build(arena, viewport, &callbacks, inputs, root_background_source, &mut state);
    state.last_tree_inputs = Some(inputs);
    state.last_root_background_source = Some(root_background_source);
    let arena_ref = unsafe { arena_from_handle(arena) };
    arena_ref.paint_state().borrow_mut().visual_context = state;
    outcome
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_apply_css_transform_to_rect(
    arena: *mut c_void,
    node: NodeSlotId,
    callbacks: FfiVisualContextHostCallbacks,
    rect: FfiCssPixelRect,
) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    let Some((transform, _is_invertible)) =
        crate::painting::visual_context::node_values::compute_transform(&arena.paintable_rows(), &callbacks, node, 1.0)
    else {
        return rect;
    };
    let origin = transform.origin;
    let mapped = transform
        .matrix
        .extract_2d_affine()
        .map_rect(
            libgfx_rust::FloatRect::new(
                rect.x.to_float(),
                rect.y.to_float(),
                rect.width.to_float(),
                rect.height.to_float(),
            )
            .translated(-origin.x, -origin.y),
        )
        .translated(origin.x, origin.y);
    FfiCssPixelRect {
        x: CssPixels::nearest_value_for_f32(mapped.x),
        y: CssPixels::nearest_value_for_f32(mapped.y),
        width: CssPixels::nearest_value_for_f32(mapped.width),
        height: CssPixels::nearest_value_for_f32(mapped.height),
    }
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
pub unsafe extern "C" fn layout_arena_set_needs_to_refresh_scroll_state(arena: *mut c_void, value: bool) {
    let arena = unsafe { arena_from_handle(arena) };
    arena
        .paint_state()
        .borrow_mut()
        .visual_context
        .needs_to_refresh_scroll_state = value;
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_scroll_state(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    let state = &mut paint_state.visual_context;
    state.scroll_state.clear();
    state.scroll_state_snapshot.clear();
    state.needs_to_refresh_scroll_state = true;
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_sticky_constraints(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let mut paint_state = arena.paint_state().borrow_mut();
    let state = &mut paint_state.visual_context;
    let mut any_sticky_payload_changed = false;
    if let Some(tree) = state.tree.as_mut() {
        any_sticky_payload_changed = crate::painting::visual_context::refresh::refresh_sticky_constraints(
            &paintable_rows,
            &state.scroll_state,
            tree,
            &callbacks.tree_inputs(),
        );
    }
    state.needs_to_refresh_scroll_state = true;
    any_sticky_payload_changed
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_scroll_state(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let mut paint_state = arena.paint_state().borrow_mut();
    let state = &mut paint_state.visual_context;
    if !state.needs_to_refresh_scroll_state {
        return;
    }
    state.needs_to_refresh_scroll_state = false;
    crate::painting::visual_context::refresh::refresh_scroll_state(
        &paintable_rows,
        &callbacks,
        &mut state.scroll_state,
    );
    state.scroll_state_snapshot = state
        .scroll_state
        .snapshot(callbacks.tree_inputs().device_pixels_per_css_pixel);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_record_display_list(
    arena: *mut c_void,
    viewport: NodeSlotId,
    callbacks: crate::painting::host::FfiHitTestHostCallbacks,
    paint_callbacks: crate::painting::host::FfiPaintHostCallbacks,
    visual_context_callbacks: crate::painting::host::FfiVisualContextHostCallbacks,
    inputs: crate::painting::host::FfiRecordingInputs,
) -> u64 {
    let arena = unsafe { arena_from_handle(arena) };
    {
        let mut paint_state = arena.paint_state().borrow_mut();
        let has_blocking_wheel_event_region_covering_viewport =
            inputs.has_blocking_wheel_event_region_covering_viewport;
        if paint_state.recorded_has_blocking_wheel_event_region_covering_viewport
            != Some(has_blocking_wheel_event_region_covering_viewport)
        {
            arena.mark_all_descendant_subtree_caches_dirty();
            paint_state.recorded_has_blocking_wheel_event_region_covering_viewport =
                Some(has_blocking_wheel_event_region_covering_viewport);
        }
        if paint_state.recorded_canvas_color != Some(inputs.canvas_color) {
            arena.mark_all_paint_caches_dirty();
            paint_state.recorded_canvas_color = Some(inputs.canvas_color);
        }
    }
    let mut output = {
        let paint_state = arena.paint_state().borrow();
        if !arena.paintable_row_is_populated(viewport) || arena.stacking_context_entries(viewport).is_none() {
            return 0;
        }
        let visual_context = &paint_state.visual_context;
        let inputs = crate::painting::record::RecordingInputs::from_host_and_last_visual_context_update(
            inputs,
            visual_context
                .last_tree_inputs
                .expect("a recording follows a visual context update"),
            visual_context
                .last_root_background_source
                .expect("a recording follows a visual context update"),
        );
        let command_cache_source = (!inputs.should_show_line_box_borders)
            .then(|| paint_state.paint_command_cache_source.clone())
            .flatten();
        arena.set_paint_recording_in_progress(true);
        let output = crate::painting::record::traversal::record_display_list(
            arena,
            &paint_state,
            viewport,
            &callbacks,
            &paint_callbacks,
            &visual_context_callbacks,
            inputs,
            paint_state.hit_test_list_generation + 1,
            command_cache_source,
            paint_state.hit_test_item_cache_source.clone(),
        );
        if crate::painting::record::verify::enabled_by_environment()
            && output.recording_stats.spliced_capture_count() > 0
            && !inputs.should_show_line_box_borders
        {
            let mut inputs_for_recording_from_scratch = inputs;
            inputs_for_recording_from_scratch.paint_command_cache_read_write = false;
            let recording_from_scratch = crate::painting::record::traversal::record_display_list(
                arena,
                &paint_state,
                viewport,
                &callbacks,
                &paint_callbacks,
                &visual_context_callbacks,
                inputs_for_recording_from_scratch,
                paint_state.hit_test_list_generation + 1,
                None,
                None,
            );
            crate::painting::record::verify::verify_spliced_recording_matches_fresh(
                arena,
                &output,
                &recording_from_scratch,
            );
        }
        arena.set_paint_recording_in_progress(false);
        output
    };
    let mut paint_state = arena.paint_state().borrow_mut();
    output.is_identical_to_cache_source = paint_state
        .paint_command_cache_source
        .as_ref()
        .zip(paint_state.hit_test_item_cache_source.as_ref())
        .is_some_and(|(source, item_source)| {
            std::rc::Rc::ptr_eq(&output.display_list, &source.display_list)
                && std::rc::Rc::ptr_eq(&output.hit_test_list.items, &item_source.items)
                && output.recorded_structural_epoch == source.recorded_structural_epoch
                && output.wheel_event_listener_state_generation == source.wheel_event_listener_state_generation
                && output.has_blocking_wheel_event_listeners == source.has_blocking_wheel_event_listeners
                && output.mask_display_lists.is_empty()
                && source.mask_display_lists.is_empty()
        });
    let list = std::mem::take(&mut output.hit_test_list);
    let previous_list_is_the_source = paint_state
        .hit_test_list
        .as_ref()
        .zip(paint_state.hit_test_item_cache_source.as_ref())
        .is_some_and(|(list, source)| std::rc::Rc::ptr_eq(&list.items, &source.items));
    if output.is_identical_to_cache_source && previous_list_is_the_source {
        drop(list);
    } else {
        paint_state.hit_test_list_generation += 1;
        debug_assert_eq!(list.generation, paint_state.hit_test_list_generation);
        if inputs.paint_command_cache_read_write {
            paint_state.hit_test_item_cache_source = Some(std::rc::Rc::new(
                crate::painting::record::cache::HitTestItemCacheSource {
                    items: list.items.clone(),
                },
            ));
        }
        paint_state.hit_test_list = Some(list);
    }
    let output = std::rc::Rc::new(output);
    if inputs.paint_command_cache_read_write {
        paint_state.paint_command_cache_source = Some(output.clone());
        // Read-only recordings commit nothing and must not age dirty stamps out.
        arena.note_paint_record_completed_with_cache_writes();
        paint_state.visual_context.quarantined_slots_are_releasable = true;
    }
    paint_state.last_recording = Some(output);
    list_generation_of(&paint_state)
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_selection_shadow(
    sink: *mut c_void,
    color: libgfx_rust::Color,
    offset_x: CssPixels,
    offset_y: CssPixels,
    blur_radius: CssPixels,
) {
    // SAFETY: `sink` is the Vec pointer handed out by
    // FfiPaintHostCallbacks::selection_style_facts.
    let shadows = unsafe { &mut *sink.cast::<Vec<crate::painting::record::paint::text::ShadowLayer>>() };
    shadows.push(crate::painting::record::paint::text::ShadowLayer {
        color: color.0,
        offset_x,
        offset_y,
        blur_radius,
    });
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
    // SAFETY: `sink` is the ColorStopSink pointer handed out by FfiPaintHostCallbacks::background_layer_image.
    let sink = unsafe { &mut *sink.cast::<crate::painting::host::ColorStopSink>() };
    sink.colors.push(color);
    sink.positions.push(position);
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
/// live for the call; `consume` is called synchronously with a recording that is only valid
/// during that call and a retained visual context tree handle the host takes ownership of.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_record_image_paint_display_list(
    inputs: *const FfiImagePaintRecordInputs,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, FfiRecordedDisplayList, *const c_void),
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
    unsafe { consume(context, (&recorded).into(), Rc::into_raw(Rc::new(tree)).cast()) };
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_stats(
    arena: *mut c_void,
) -> crate::painting::host::FfiPaintRecordingStats {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .last_recording
        .as_ref()
        .map_or_else(Default::default, |recording| recording.recording_stats)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_is_identical_to_cache_source(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .last_recording
        .as_ref()
        .is_some_and(|recording| recording.is_identical_to_cache_source)
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
    let arena = unsafe { arena_from_handle(arena) };
    if propagated_text_decorations {
        arena.invalidate_propagated_text_decoration_caches(paintable);
    } else {
        arena.invalidate_paint_cache(paintable);
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_invalidate_for_repaint(arena: *mut c_void, paintable: NodeSlotId) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.invalidate_for_repaint(paintable);
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
    arena.invalidate_subtree_for_repaint(paintable);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_invalidate_all_paint_caches(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.mark_all_paint_caches_dirty();
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
pub unsafe extern "C" fn layout_arena_paintable_dump_block_fragments(
    arena: *mut c_void,
    paintable: NodeSlotId,
    indent: usize,
    interactive: bool,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(paintable) {
        return;
    }
    let mut out = Vec::new();
    crate::painting::dump::dump_block_fragments(&mut out, &paintable_rows, paintable, indent, interactive);
    if !out.is_empty() {
        // SAFETY: The consumer copies the byte span synchronously.
        unsafe { consume(context, out.as_ptr(), out.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_dump_inline_piece_fragments(
    arena: *mut c_void,
    inline_paintable: NodeSlotId,
    indent: usize,
    interactive: bool,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(inline_paintable) {
        return;
    }
    let mut out = Vec::new();
    crate::painting::dump::dump_inline_piece_fragments(
        &mut out,
        &paintable_rows,
        inline_paintable,
        indent,
        interactive,
    );
    if !out.is_empty() {
        // SAFETY: The consumer copies the byte span synchronously.
        unsafe { consume(context, out.as_ptr(), out.len()) };
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
    context: *mut c_void,
    consume: unsafe extern "C" fn(
        *mut c_void,
        *const grid_formatting_context::FfiUsedGridTrackList,
        *const grid_formatting_context::FfiUsedGridTrackList,
    ),
) {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(paintable) {
        return;
    }
    if let Some(tracks) = crate::painting::paintable_geometry::committed_used_grid_tracks(arena, paintable) {
        tracks.with_ffi_views(|columns, rows| unsafe { consume(context, columns, rows) });
    }
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
/// `tree` must be a retained tree handle (from `layout_arena_main_visual_context_tree_retain`,
/// a host callback, or an earlier retain), used on the thread that owns it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_retain(tree: *const c_void) -> *const c_void {
    // SAFETY: The caller guarantees `tree` is a live retained handle, so the strong count is at least one.
    unsafe { Rc::increment_strong_count(tree.cast::<crate::painting::visual_context::VisualContextTree>()) };
    tree
}

/// # Safety
///
/// `tree` must be null or a retained tree handle that the caller gives up with this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_release(tree: *const c_void) {
    if tree.is_null() {
        return;
    }
    // SAFETY: The caller gives up the reference it retained, and the strong count is at least one.
    unsafe { Rc::decrement_strong_count(tree.cast::<crate::painting::visual_context::VisualContextTree>()) };
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_structural_epoch(tree: *const c_void) -> u64 {
    unsafe { tree_from_handle(tree) }.structural_epoch
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `append` is called synchronously with `sink`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_serialize(
    tree: *const c_void,
    sink: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let bytes = unsafe { tree_from_handle(tree) }.to_bytes();
    // SAFETY: The C++ sink copies the bytes synchronously.
    unsafe { append(sink, bytes.as_ptr(), bytes.len()) };
}

/// # Safety
///
/// `bytes` must address `length` readable bytes for the call. Returns a retained tree handle the
/// caller owns, or null when the bytes do not describe a valid tree.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_deserialize(bytes: *const u8, length: usize) -> *const c_void {
    // SAFETY: The caller guarantees `bytes` addresses `length` readable bytes.
    let bytes = unsafe { ffi_slice(bytes, length) };
    match crate::painting::visual_context::VisualContextTree::from_bytes(bytes) {
        Some(tree) => Rc::into_raw(Rc::new(tree)).cast(),
        None => std::ptr::null(),
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_spatial_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.spatial_nodes.len()
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_frame_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.frame_nodes.len()
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_live_spatial_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.live_spatial_node_count as usize
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_live_frame_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.live_frame_node_count as usize
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `command_runs` must address `command_run_count`
/// runs and `mask_frames` `mask_frame_count` frame indices for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_references_only_live_visual_context_nodes(
    tree: *const c_void,
    command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    command_run_count: usize,
    mask_frames: *const FrameNodeIndex,
    mask_frame_count: usize,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the slices address the stated number of values.
    let (command_runs, mask_frames) = unsafe {
        (
            ffi_slice(command_runs, command_run_count),
            ffi_slice(mask_frames, mask_frame_count),
        )
    };
    tree.display_list_references_only_live_nodes(command_runs, mask_frames)
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len`
/// points and `out_local_point` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_transform_point_for_hit_test(
    tree: *const c_void,
    context: crate::painting::display_list::commands::ContextRef,
    screen_point: libgfx_rust::FloatPoint,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    respect_clip: bool,
    out_local_point: *mut libgfx_rust::FloatPoint,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    let clip_behavior = crate::painting::visual_context::ClipBehavior::from_respect_clip(respect_clip);
    match tree.transform_point_for_hit_test(context, screen_point, scroll_offsets, clip_behavior) {
        Some(local_point) => {
            // SAFETY: The caller guarantees `out_local_point` is writable.
            unsafe { *out_local_point = local_point };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_inverse_transform_point(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    screen_point: libgfx_rust::FloatPoint,
) -> libgfx_rust::FloatPoint {
    unsafe { tree_from_handle(tree) }.inverse_transform_point(spatial, screen_point)
}

fn include_visual_viewport_transform_from(
    include: bool,
) -> crate::painting::visual_context::IncludeVisualViewportTransform {
    if include {
        crate::painting::visual_context::IncludeVisualViewportTransform::Yes
    } else {
        crate::painting::visual_context::IncludeVisualViewportTransform::No
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_transform_rect_to_viewport(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    rect: libgfx_rust::FloatRect,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    include_visual_viewport_transform: bool,
) -> libgfx_rust::FloatRect {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    tree.transform_rect_to_viewport(
        spatial,
        rect,
        scroll_offsets,
        include_visual_viewport_transform_from(include_visual_viewport_transform),
    )
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_cumulative_scroll_chain_offset(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
) -> libgfx_rust::FloatPoint {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    tree.cumulative_scroll_chain_offset(spatial, scroll_offsets)
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_accumulated_matrix(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    include_visual_viewport_transform: bool,
) -> libgfx_rust::FloatMatrix4x4 {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    tree.accumulated_matrix(
        spatial,
        scroll_offsets,
        include_visual_viewport_transform_from(include_visual_viewport_transform),
    )
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len`
/// points, and `push` is called synchronously with `sink` for every sticky node's resolved entry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_resolve_sticky_offsets(
    tree: *const c_void,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    sink: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, SpatialNodeIndex, libgfx_rust::FloatPoint),
) {
    let resolved_sticky_entries = {
        let tree = unsafe { tree_from_handle(tree) };
        // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points, and the
        // borrow ends before the sink may grow the same storage.
        let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
        tree.resolve_sticky_offsets(scroll_offsets)
    };
    for (node_index, offset) in resolved_sticky_entries {
        // SAFETY: The C++ sink records the entry synchronously.
        unsafe { push(sink, node_index, offset) };
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_visual_viewport_transform(
    tree: *const c_void,
) -> crate::painting::host::FfiVisualViewportTransform {
    let tree = unsafe { tree_from_handle(tree) };
    let crate::painting::visual_context::SpatialData::Transform(transform) =
        &tree.spatial_nodes[crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX.0 as usize].data
    else {
        unreachable!("the visual viewport node is a transform");
    };
    crate::painting::host::FfiVisualViewportTransform {
        matrix: transform.matrix,
        origin: transform.origin,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle. Returns a retained handle to a copy of the tree whose
/// visual viewport node carries the given transform; the copy keeps the structural epoch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_with_visual_viewport_transform(
    tree: *const c_void,
    transform: crate::painting::host::FfiVisualViewportTransform,
) -> *const c_void {
    let mut copy = unsafe { tree_from_handle(tree) }.clone();
    copy.set_visual_viewport_matrix_and_origin(transform.matrix, transform.origin);
    Rc::into_raw(Rc::new(copy)).cast()
}

/// # Safety
///
/// `tree` must be a live retained tree handle; each sample array must address its stated count, and
/// each non-empty filter sample must address its stated byte count. Returns a retained handle to a
/// copy of the tree carrying the sampled values; the copy keeps the structural epoch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_with_sampled_values(
    tree: *const c_void,
    frame_opacities: *const crate::painting::host::FfiFrameOpacitySample,
    frame_opacity_count: usize,
    frame_background_colors: *const crate::painting::host::FfiFrameBackgroundColorSample,
    frame_background_color_count: usize,
    frame_filters: *const crate::painting::host::FfiFrameFilterSample,
    frame_filter_count: usize,
    spatial_matrices: *const crate::painting::host::FfiSpatialTransformSample,
    spatial_matrix_count: usize,
) -> *const c_void {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the sample arrays address the stated counts.
    let (frame_opacities, frame_background_colors, frame_filters, spatial_matrices) = unsafe {
        (
            ffi_slice(frame_opacities, frame_opacity_count),
            ffi_slice(frame_background_colors, frame_background_color_count),
            ffi_slice(frame_filters, frame_filter_count),
            ffi_slice(spatial_matrices, spatial_matrix_count),
        )
    };
    let frame_opacities: Vec<(FrameNodeIndex, f32)> = frame_opacities
        .iter()
        .map(|sample| (FrameNodeIndex(sample.frame), sample.opacity))
        .collect();
    let frame_background_colors: Vec<(FrameNodeIndex, libgfx_rust::Color)> = frame_background_colors
        .iter()
        .map(|sample| (FrameNodeIndex(sample.frame), sample.color))
        .collect();
    let frame_filters: Vec<(FrameNodeIndex, Option<Rc<Vec<u8>>>)> = frame_filters
        .iter()
        .map(|sample| {
            let filter = if sample.filter_size == 0 {
                None
            } else {
                // SAFETY: The caller guarantees each filter byte range addresses the stated size.
                Some(Rc::new(
                    unsafe { ffi_slice(sample.filter_bytes, sample.filter_size) }.to_vec(),
                ))
            };
            (FrameNodeIndex(sample.frame), filter)
        })
        .collect();
    let spatial_matrices: Vec<(SpatialNodeIndex, libgfx_rust::FloatMatrix4x4)> = spatial_matrices
        .iter()
        .map(|sample| (SpatialNodeIndex(sample.spatial), sample.matrix))
        .collect();
    let sampled = tree.with_sampled_visual_animation_values(
        &frame_opacities,
        &frame_background_colors,
        &frame_filters,
        &spatial_matrices,
    );
    Rc::into_raw(Rc::new(sampled)).cast()
}

/// # Safety
///
/// `tree` must be a live retained tree handle and `color` must point to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_sampled_background_color(
    tree: *const c_void,
    frame: FrameNodeIndex,
    color: *mut libgfx_rust::Color,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    let Some(sampled_color) = tree.sampled_background_color(frame) else {
        return false;
    };
    // SAFETY: The caller guarantees that `color` points to writable storage.
    unsafe { *color = sampled_color };
    true
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `targets` must address `target_count` node indices.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_visual_animation_targets_are_valid(
    tree: *const c_void,
    target_kind: crate::painting::host::FfiVisualAnimationTargetKind,
    targets: *const u32,
    target_count: usize,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the targets address `target_count` indices.
    let targets = unsafe { ffi_slice(targets, target_count) };
    tree.visual_animation_targets_are_valid(target_kind, targets)
}

/// # Safety
///
/// `tree` must be a live retained tree handle and `out_opacity` must be writable. Returns whether
/// `frame` names an effects frame.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_effects_opacity(
    tree: *const c_void,
    frame: FrameNodeIndex,
    out_opacity: *mut f32,
) -> bool {
    match unsafe { tree_from_handle(tree) }.effects_opacity(frame) {
        Some(opacity) => {
            // SAFETY: The caller guarantees `out_opacity` is writable.
            unsafe { *out_opacity = opacity };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `roots` must address `root_count` spatial indices and
/// `out_flags` must address one writable flag per spatial node (`flag_count` of them).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_mark_spatial_subtrees(
    tree: *const c_void,
    roots: *const SpatialNodeIndex,
    root_count: usize,
    out_flags: *mut bool,
    flag_count: usize,
) {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the roots address `root_count` indices.
    let roots = unsafe { ffi_slice(roots, root_count) };
    let in_subtree = tree.spatial_nodes_in_subtrees_of(roots);
    assert_eq!(in_subtree.len(), flag_count);
    // SAFETY: The caller guarantees `out_flags` addresses `flag_count` writable flags.
    unsafe { std::ptr::copy_nonoverlapping(in_subtree.as_ptr(), out_flags, flag_count) };
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_has_unisolated_blending_frame(tree: *const c_void) -> bool {
    unsafe { tree_from_handle(tree) }.has_unisolated_blending_frame()
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `visit` is called synchronously with `context` for every
/// effects frame that carries a filter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_for_each_effects_filter_bytes(
    tree: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let tree = unsafe { tree_from_handle(tree) };
    for frame in &tree.frame_nodes {
        if let crate::painting::visual_context::FrameData::Effects(effects) = &frame.data
            && let Some(filter_bytes) = &effects.filter
        {
            // SAFETY: The C++ visitor reads the bytes synchronously.
            unsafe { visit(context, filter_bytes.as_ptr(), filter_bytes.len()) };
        }
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_frame_is_isolated_by_layer_frame(
    tree: *const c_void,
    frame: FrameNodeIndex,
) -> bool {
    unsafe { tree_from_handle(tree) }.frame_is_isolated_by_layer_frame(frame)
}

/// # Safety
///
/// Returns a builder handle for hand-built test trees rooted at an identity visual viewport
/// transform; every builder call must receive it until `visual_context_tree_test_builder_finish`
/// or `visual_context_tree_test_builder_destroy` consumes it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_create() -> *mut c_void {
    let tree =
        crate::painting::visual_context::VisualContextTree::create(crate::painting::visual_context::TransformData {
            matrix: libgfx_rust::FloatMatrix4x4::identity(),
            origin: libgfx_rust::FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: crate::painting::visual_context::TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        });
    Box::into_raw(Box::new(tree)).cast()
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
unsafe fn test_builder_tree<'a>(builder: *mut c_void) -> &'a mut crate::painting::visual_context::VisualContextTree {
    // SAFETY: The caller guarantees `builder` is the live boxed tree the create call returned.
    unsafe { &mut *builder.cast::<crate::painting::visual_context::VisualContextTree>() }
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_transform(
    builder: *mut c_void,
    parent: u32,
    matrix: libgfx_rust::FloatMatrix4x4,
    origin: libgfx_rust::FloatPoint,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_spatial(
        crate::painting::visual_context::SpatialData::Transform(crate::painting::visual_context::TransformData {
            matrix,
            origin,
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: crate::painting::visual_context::TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }),
        SpatialNodeIndex(parent),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_scroll(builder: *mut c_void, parent: u32) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_spatial(
        crate::painting::visual_context::SpatialData::Scroll(crate::painting::visual_context::ScrollData {
            state_slot: crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: SpatialNodeIndex(parent),
        }),
        SpatialNodeIndex(parent),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_sticky(
    builder: *mut c_void,
    parent: u32,
    constraints: crate::painting::host::FfiTestStickyConstraints,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    let inset = |value: crate::painting::display_list::commands::OptionalF32| value.has_value.then_some(value.value);
    tree.append_spatial(
        crate::painting::visual_context::SpatialData::Sticky(crate::painting::visual_context::StickyData {
            scroller: SpatialNodeIndex(constraints.scroller),
            parent_sticky: constraints
                .has_parent_sticky
                .then_some(SpatialNodeIndex(constraints.parent_sticky)),
            position_relative_to_scroller: constraints.position_relative_to_scroller,
            border_box_size: constraints.border_box_size,
            scrollport_size: constraints.scrollport_size,
            containing_block_region: constraints.containing_block_region,
            needs_parent_offset_adjustment: constraints.needs_parent_offset_adjustment,
            inset_top: inset(constraints.inset_top),
            inset_right: inset(constraints.inset_right),
            inset_bottom: inset(constraints.inset_bottom),
            inset_left: inset(constraints.inset_left),
            state_slot: crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: if constraints.has_parent_sticky {
                SpatialNodeIndex(constraints.parent_sticky)
            } else {
                SpatialNodeIndex(constraints.scroller)
            },
        }),
        SpatialNodeIndex(parent),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_clip_frame(
    builder: *mut c_void,
    parent_frame: u32,
    spatial: u32,
    rect: libgfx_rust::FloatRect,
    corner_radii: libgfx_rust::CornerRadii,
    mode: crate::painting::visual_context::ClipMode,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_frame(
        crate::painting::visual_context::FrameData::Clip(crate::painting::visual_context::ClipData {
            rect,
            corner_radii,
            mode,
        }),
        FrameNodeIndex(parent_frame),
        SpatialNodeIndex(spatial),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_background_color_animation_frame(
    builder: *mut c_void,
    parent_frame: u32,
    spatial: u32,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_frame(
        crate::painting::visual_context::FrameData::BackgroundColorAnimation,
        FrameNodeIndex(parent_frame),
        SpatialNodeIndex(spatial),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`; `path_bytes`
/// must address `path_bytes_length` readable bytes of a serialized `Gfx::Path`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_clip_path_frame(
    builder: *mut c_void,
    parent_frame: u32,
    spatial: u32,
    path_bytes: *const u8,
    path_bytes_length: usize,
    bounding_rect: libgfx_rust::IntRect,
    fill_rule: libgfx_rust::WindingRule,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    // SAFETY: The caller guarantees `path_bytes` addresses `path_bytes_length` readable bytes.
    let path_bytes = unsafe { ffi_slice(path_bytes, path_bytes_length) };
    let path = libgfx_rust::path::OwnedPath::from_serialized_bytes(path_bytes);
    tree.append_frame(
        crate::painting::visual_context::FrameData::ClipPath(crate::painting::visual_context::ClipPathData {
            path: Rc::new(path),
            bounding_rect,
            fill_rule,
        }),
        FrameNodeIndex(parent_frame),
        SpatialNodeIndex(spatial),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_effects_frame(
    builder: *mut c_void,
    parent_frame: u32,
    spatial: u32,
    opacity: f32,
    blend_mode: libgfx_rust::CompositingAndBlendingOperator,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_frame(
        crate::painting::visual_context::FrameData::Effects(crate::painting::visual_context::EffectsData {
            opacity,
            blend_mode,
            filter: None,
        }),
        FrameNodeIndex(parent_frame),
        SpatialNodeIndex(spatial),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`. Gives the tree
/// the structural epoch another tree carries, so a test can stage a compatible tree-only update.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_set_structural_epoch(
    builder: *mut c_void,
    structural_epoch: u64,
) {
    let tree = unsafe { test_builder_tree(builder) };
    tree.structural_epoch = structural_epoch;
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`, consumed by this
/// call. Returns a retained tree handle the caller owns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_finish(builder: *mut c_void) -> *const c_void {
    // SAFETY: The caller hands over the boxed tree the create call returned.
    let tree = unsafe { Box::from_raw(builder.cast::<crate::painting::visual_context::VisualContextTree>()) };
    Rc::into_raw(Rc::new(*tree)).cast()
}

/// # Safety
///
/// `builder` must be null or a live handle from `visual_context_tree_test_builder_create`, consumed
/// by this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_destroy(builder: *mut c_void) {
    if builder.is_null() {
        return;
    }
    // SAFETY: The caller hands over the boxed tree the create call returned.
    drop(unsafe { Box::from_raw(builder.cast::<crate::painting::visual_context::VisualContextTree>()) });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread; `out`
/// must have room for `capacity` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_state_snapshot(
    arena: *mut c_void,
    out: *mut libgfx_rust::FloatPoint,
    capacity: usize,
) -> usize {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    let snapshot = &paint_state.visual_context.scroll_state_snapshot;
    let needed = snapshot.len();
    if !out.is_null() {
        for (index, offset) in snapshot.iter().enumerate() {
            if index >= capacity {
                break;
            }
            // SAFETY: within the caller's capacity.
            unsafe { *out.add(index) = *offset };
        }
    }
    needed
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
    with_hit_test_list_and_derived_structures(arena, Default::default(), |list, arena| {
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// returned pointers borrow the last recording and stay valid until the next one replaces it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_recorded_display_list(arena: *mut c_void) -> FfiRecordedDisplayList {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .last_recording
        .as_ref()
        .map_or_else(FfiRecordedDisplayList::empty, |recording| {
            FfiRecordedDisplayList::with_mask_registrations(&recording.display_list, &recording.mask_display_lists)
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
    with_hit_test_list_and_derived_structures(arena, Default::default(), |list, _| {
        let line = &list.caret_lines[line_index];
        crate::painting::host::FfiCaretLineExport {
            rect: line.rect.into(),
            context: line.context,
            first_caret_item_index: line.first_caret_item_index,
            last_caret_item_index: line.last_caret_item_index,
        }
    })
}

fn list_generation_of(paint_state: &crate::painting::paint_state::PaintState) -> u64 {
    paint_state.hit_test_list.as_ref().map_or(0, |list| list.generation)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_list_generation(arena: *mut c_void) -> u64 {
    let arena = unsafe { arena_from_handle(arena) };
    list_generation_of(&arena.paint_state().borrow())
}

fn with_hit_test_list_items_only<R>(
    arena: *mut c_void,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::layout::LayoutNodeArena) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    let Some(list) = paint_state.hit_test_list.as_ref() else {
        return default;
    };
    query(list, arena)
}

fn with_hit_test_list_and_derived_structures<R>(
    arena: *mut c_void,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::layout::LayoutNodeArena) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    let Some(list) = paint_state.hit_test_list.as_mut() else {
        return default;
    };
    list.build_derived_structures_if_needed();
    query(list, arena)
}

fn with_hit_test_list_and_derived_structures_and_visual_context_tree<R>(
    arena: *mut c_void,
    default: R,
    query: impl FnOnce(
        &crate::painting::hit_test::HitTestList,
        &crate::painting::visual_context::VisualContextTree,
        &crate::layout::LayoutNodeArena,
    ) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    let crate::painting::paint_state::PaintState {
        hit_test_list,
        visual_context,
        ..
    } = &mut *paint_state;
    let Some(list) = hit_test_list.as_mut() else {
        return default;
    };
    list.build_derived_structures_if_needed();
    let Some(tree) = visual_context.tree.as_deref() else {
        return default;
    };
    query(list, tree, arena)
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
    with_hit_test_list_and_derived_structures_and_visual_context_tree(arena, Default::default(), |list, tree, arena| {
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
    with_hit_test_list_and_derived_structures_and_visual_context_tree(arena, Default::default(), |list, tree, arena| {
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
        with_hit_test_list_and_derived_structures_and_visual_context_tree(arena, Vec::new(), |list, tree, arena| {
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
    with_hit_test_list_and_derived_structures(arena, usize::MAX, |list, _| {
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
    with_hit_test_list_and_derived_structures(arena, Default::default(), |list, _| {
        match list.caret_item_for_line(
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
    with_hit_test_list_and_derived_structures(arena, 0, |list, _| list.line_block_coordinate(line_index).raw_value())
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
    with_hit_test_list_and_derived_structures(arena, false, |list, _| {
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
    with_hit_test_list_and_derived_structures_and_visual_context_tree(arena, Default::default(), |list, tree, arena| {
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
            block_container_margin_rect: closest.block_container_margin_rect.map(Into::into).into(),
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
    with_hit_test_list_and_derived_structures(arena, Default::default(), |list, arena| {
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
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_push_line_break_caret_target(
    sink: *mut c_void,
    target: crate::painting::host::FfiLineBreakCaretTarget,
) {
    // SAFETY: `sink` is the Vec pointer handed out by FfiHitTestHostCallbacks::line_break_caret_targets.
    let targets = unsafe { &mut *sink.cast::<Vec<crate::painting::host::FfiLineBreakCaretTarget>>() };
    targets.push(target);
}
