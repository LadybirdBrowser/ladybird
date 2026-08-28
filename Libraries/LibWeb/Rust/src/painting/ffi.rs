/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::css::css_pixels::CssPixels;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::used_values::FfiCssPixelSize;
use crate::layout::{grid_formatting_context, svg_formatting_context, used_values};
use crate::painting::host::FfiRecordedDisplayList;
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::PaintableRowsRead;
use std::ffi::c_void;

/// SAFETY: `arena` must be a live handle from `layout_arena_create`, borrowed for this call on
/// the document thread.
unsafe fn arena_from_handle<'a>(arena: *mut c_void) -> &'a LayoutNodeArena {
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
    abort_on_panic(|| {
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
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_physical_resize_axes(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiPhysicalResizeAxes {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let axes = crate::painting::chrome_geometry::physical_resize_axes(&arena.paintable_rows(), slot);
        FfiPhysicalResizeAxes {
            horizontal: axes.horizontal,
            vertical: axes.vertical,
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_is_chrome_mirrored(arena: *mut c_void, slot: NodeSlotId) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        crate::painting::chrome_geometry::is_chrome_mirrored(&arena.paintable_rows(), slot)
    })
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
    abort_on_panic(|| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_minimum_scroll_offset(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        crate::painting::chrome_geometry::minimum_scroll_offset(&arena.paintable_rows(), slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_maximum_scroll_offset(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        crate::painting::chrome_geometry::maximum_scroll_offset(&arena.paintable_rows(), slot).into()
    })
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
    abort_on_panic(|| {
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
    })
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.set_chrome_state_callback(context, callback);
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_chrome_state_callback(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.clear_chrome_state_callback();
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_row(arena: *mut c_void, slot: NodeSlotId) -> *const PaintableData {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return std::ptr::null();
        }
        paintable_rows.paintable_data_ptr(slot)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_cleared_from_node(arena: *mut c_void, layout_node: NodeSlotId) {
    abort_on_panic(|| {
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
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_event_dispatch_node_shell(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> *mut c_void {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        let mut current = paintable_rows.paintable_row_is_populated(slot).then_some(slot);
        while let Some(paintable) = current {
            let layout_node = paintable;
            let flags = arena.node_flags_if_live(layout_node);
            if flags & crate::layout::node_data::NodeFlag::Anonymous as u32 == 0 {
                return arena.shell_if_live(layout_node);
            }
            current = crate::painting::paint_order::paint_parent(&paintable_rows, paintable);
        }
        std::ptr::null_mut()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_layout_node_shell(arena: *mut c_void, slot: NodeSlotId) -> *mut c_void {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(slot) {
            return std::ptr::null_mut();
        }
        arena.shell_if_live(slot)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_has_child_paintables(arena: *mut c_void, slot: NodeSlotId) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        crate::painting::paint_order::first_paint_child(&arena.paintable_rows(), slot).is_some()
    })
}

unsafe fn ffi_slice<'a, T>(data: *const T, length: usize) -> &'a [T] {
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle_mut(arena) };
        if !arena.paintable_row_is_populated(viewport) {
            return;
        }
        // SAFETY: The caller guarantees the entry span is valid for this synchronous call.
        let entries = unsafe { ffi_slice(entries, entry_count) };
        crate::painting::selection::apply(&mut arena.paintable_rows_mut(), viewport, entries);
        arena.paint_state().borrow_mut().selection = Some(crate::painting::selection::SelectionRange {
            start_offset: range_start_offset,
            end_offset: range_end_offset,
        });
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_selection_clear(arena: *mut c_void, viewport: NodeSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle_mut(arena) };
        if !arena.paintable_row_is_populated(viewport) {
            return;
        }
        crate::painting::selection::clear(&mut arena.paintable_rows_mut(), viewport);
        arena.paint_state().borrow_mut().selection = None;
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_set_sticky_insets(
    arena: *mut c_void,
    slot: NodeSlotId,
    insets: FfiStickyInsets,
    has_insets: bool,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle_mut(arena) };
        let mut paintable_rows = arena.paintable_rows_mut();
        if paintable_rows.paintable_row_is_populated(slot) {
            let data = paintable_rows.paintable_data_mut(slot);
            data.sticky_insets = insets;
            data.has_sticky_insets = has_insets;
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_clear_overflow_data(arena: *mut c_void, slot: NodeSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle_mut(arena) };
        let mut paintable_rows = arena.paintable_rows_mut();
        if paintable_rows.paintable_row_is_populated(slot) {
            paintable_rows.paintable_data_mut(slot).overflow_measured_this_commit = false;
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_clear_cached_overflow_data(arena: *mut c_void, slot: NodeSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintable_rows().clear_cached_overflow_data(slot);
    });
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
    abort_on_panic(|| {
        let assignments = {
            let arena = unsafe { arena_from_handle(arena) };
            let paintable_rows = arena.paintable_rows();
            if !paintable_rows.paintable_row_is_populated(box_paintable) {
                return;
            }
            let paint_state = arena.paint_state().borrow();
            crate::painting::scrollable_overflow::measure_scrollable_overflow(
                &paintable_rows,
                &paint_state.scrollable_overflow_contained_boxes,
                &visual_context_callbacks,
                &overflow_callbacks,
                box_paintable,
            )
        };
        let arena = unsafe { arena_from_handle_mut(arena) };
        let mut paintable_rows = arena.paintable_rows_mut();
        for assignment in assignments {
            assignment.apply(&mut paintable_rows);
        }
    });
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelPoint::default();
        }
        crate::painting::paintable_geometry::committed_offset(&paintable_rows, slot)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_content_size(arena: *mut c_void, slot: NodeSlotId) -> FfiCssPixelSize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelSize::default();
        }
        crate::painting::paintable_geometry::committed_content_size(&paintable_rows, slot)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_svg_viewport_size(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelSize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelSize::default();
        }
        crate::painting::paintable_geometry::committed_svg_viewport_size(&paintable_rows, slot)
    })
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
    abort_on_panic(|| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_transform_reference_box(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        let Some(style) = arena.node_style_if_live(slot) else {
            return FfiCssPixelRect::default();
        };
        let paintable_rows = arena.paintable_rows();
        crate::painting::visual_context::node_values::transform_reference_box(style, &paintable_rows, slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_svg_viewport_user_rect(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> used_values::OptionalCssPixelRect {
    abort_on_panic(|| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_box_model(arena: *mut c_void, slot: NodeSlotId) -> FfiBoxModelMetrics {
    abort_on_panic(|| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_is_positioned(arena: *mut c_void, slot: NodeSlotId) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(slot) {
            return false;
        }
        crate::painting::style_queries::is_positioned(arena, slot)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_uses_collapsing_borders_model(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(slot) {
            return false;
        }
        crate::painting::paintable_geometry::committed_uses_collapsing_borders_model(arena, slot)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_rect(arena: *mut c_void, slot: NodeSlotId) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_rect(&paintable_rows, slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_padding_box_rect(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_padding_box_rect(&paintable_rows, slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_border_box_rect(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_border_box_rect(&paintable_rows, slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_rebuild_scrollable_overflow_contained_boxes(
    arena: *mut c_void,
    root: NodeSlotId,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        let mut paint_state = arena.paint_state().borrow_mut();
        crate::painting::scrollable_overflow::refill_contained_boxes_index(
            &paintable_rows,
            root,
            &mut paint_state.scrollable_overflow_contained_boxes,
        );
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_scrollable_overflow_contained_boxes(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena
            .paint_state()
            .borrow_mut()
            .scrollable_overflow_contained_boxes
            .clear();
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_physical_overflow_directions(
    arena: *mut c_void,
    paintable: NodeSlotId,
) -> FfiPhysicalOverflowDirections {
    abort_on_panic(|| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_build_stacking_context_tree(arena: *mut c_void, root: NodeSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle_mut(arena) };
        let mut paintable_rows = arena.paintable_rows_mut();
        let tree = {
            if !paintable_rows.paintable_row_is_populated(root) {
                return;
            }
            crate::painting::stacking_context::build_stacking_context_tree(&mut paintable_rows, root)
        };
        paintable_rows.paint_state().borrow_mut().stacking_context_tree = Some(tree);
        crate::painting::fragment_ownership::assign_fragment_ownership(&paintable_rows, root);
    });
}

use crate::painting::host::FfiVisualContextHostCallbacks;

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_assign_accumulated_visual_contexts(
    arena: *mut c_void,
    viewport: NodeSlotId,
    callbacks: FfiVisualContextHostCallbacks,
    force_incompatible_rebuild: bool,
) -> bool {
    abort_on_panic(|| {
        let (mut tree, scroll_state, paintables_with_mask_nodes, previous_assignments, assignments) = {
            let arena = unsafe { arena_from_handle(arena) };
            let paintable_rows = arena.paintable_rows();
            if !paintable_rows.paintable_row_is_populated(viewport) {
                return false;
            }
            let previous_assignments = paintable_rows.visual_context_assignments();
            let (tree, scroll_state, paintables_with_mask_nodes, assignments) =
                crate::painting::visual_context::build::build_visual_context_tree(
                    &paintable_rows,
                    &callbacks,
                    viewport,
                );
            (
                tree,
                scroll_state,
                paintables_with_mask_nodes,
                previous_assignments,
                assignments,
            )
        };
        let arena = unsafe { arena_from_handle_mut(arena) };
        let assignments_changed = {
            let mut paintable_rows = arena.paintable_rows_mut();
            for assignment in assignments {
                assignment.apply(&mut paintable_rows);
            }
            previous_assignments != paintable_rows.visual_context_assignments()
        };
        let mut paint_state = arena.paint_state().borrow_mut();
        let state = &mut paint_state.visual_context;
        state.build_count += 1;
        let is_compatible = !force_incompatible_rebuild
            && state
                .tree
                .as_ref()
                .is_some_and(|previous| tree.is_compatible_with(previous));
        tree.reused_previous_version = is_compatible;
        tree.version = if is_compatible {
            state.tree_version()
        } else {
            state.allocate_tree_version()
        };
        state.tree = Some(tree);
        state.paintables_with_mask_nodes = paintables_with_mask_nodes;
        state.scroll_state = scroll_state;
        state.scroll_state_snapshot.clear();
        state.needs_to_refresh_scroll_state = true;
        if assignments_changed {
            arena.mark_all_descendant_subtree_caches_dirty();
        }
        is_compatible
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the
/// document thread; `out_matrix` and `out_origin` must hold 16 and 2 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_compute_css_transform(
    arena: *mut c_void,
    node: NodeSlotId,
    callbacks: FfiVisualContextHostCallbacks,
    pixel_ratio: f64,
    out_matrix: *mut f32,
    out_origin: *mut f32,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let Some((transform, _is_invertible)) = crate::painting::visual_context::node_values::compute_transform(
            &arena.paintable_rows(),
            &callbacks,
            node,
            pixel_ratio,
        ) else {
            return false;
        };
        for (index, value) in transform.matrix.elements.into_iter().flatten().enumerate() {
            // SAFETY: the caller warrants 16 floats behind out_matrix.
            unsafe { out_matrix.add(index).write(value) };
        }
        // SAFETY: the caller warrants 2 floats behind out_origin.
        unsafe {
            out_origin.write(transform.origin.x);
            out_origin.add(1).write(transform.origin.y);
        }
        true
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_visual_context_values(
    arena: *mut c_void,
    paintable: NodeSlotId,
    callbacks: FfiVisualContextHostCallbacks,
) -> bool {
    abort_on_panic(|| {
        let pixel_ratio = callbacks.tree_inputs().device_pixels_per_css_pixel;
        let (compatible, has_non_invertible_css_transform) = {
            let arena = unsafe { arena_from_handle(arena) };
            let paintable_rows = arena.paintable_rows();
            if !paintable_rows.paintable_row_is_populated(paintable) {
                return false;
            }
            let mut paint_state = arena.paint_state().borrow_mut();
            let Some(tree) = paint_state.visual_context.tree.as_mut() else {
                return false;
            };
            crate::painting::visual_context::build::update_visual_context_values(
                &paintable_rows,
                &callbacks,
                tree,
                paintable,
                pixel_ratio,
            )
        };
        if let Some(value) = has_non_invertible_css_transform {
            let arena = unsafe { arena_from_handle_mut(arena) };
            arena.paintable_rows_mut().paintable_data_mut(paintable).set_flag(
                crate::painting::paintable_data::PaintableFlag::HasNonInvertibleCssTransform,
                value,
            );
        }
        compatible
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_visual_viewport_transform(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paint_state = arena.paint_state().borrow_mut();
        let Some(tree) = &mut paint_state.visual_context.tree else {
            return false;
        };
        let inputs = callbacks.tree_inputs();
        tree.set_visual_viewport_transform(
            crate::painting::visual_context::node_values::visual_viewport_transform_data(&inputs),
        );
        true
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_needs_to_refresh_scroll_state(arena: *mut c_void, value: bool) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena
            .paint_state()
            .borrow_mut()
            .visual_context
            .needs_to_refresh_scroll_state = value;
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_scroll_state(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paint_state = arena.paint_state().borrow_mut();
        let state = &mut paint_state.visual_context;
        state.scroll_state.clear();
        state.scroll_state_snapshot.clear();
        state.needs_to_refresh_scroll_state = true;
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_sticky_constraints(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        let mut paint_state = arena.paint_state().borrow_mut();
        let state = &mut paint_state.visual_context;
        if let Some(tree) = state.tree.as_mut() {
            crate::painting::visual_context::refresh::refresh_sticky_constraints(
                &paintable_rows,
                &state.scroll_state,
                tree,
                callbacks.tree_inputs().device_pixels_per_css_pixel,
            );
        }
        state.needs_to_refresh_scroll_state = true;
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_scroll_state(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) {
    abort_on_panic(|| {
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
    });
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut output = {
            let paint_state = arena.paint_state().borrow();
            if !arena.paintable_row_is_populated(viewport) || paint_state.stacking_context_tree.is_none() {
                return 0;
            }
            let command_cache_source = (!inputs.should_show_line_box_borders)
                .then(|| paint_state.paint_command_cache_source.clone())
                .flatten();
            arena.set_paint_recording_in_progress(true);
            let output = crate::painting::record::traversal::record_display_list(
                arena,
                &paint_state,
                &callbacks,
                &paint_callbacks,
                &visual_context_callbacks,
                inputs,
                paint_state.hit_test_list_generation + 1,
                command_cache_source,
                paint_state.hit_test_item_cache_source.clone(),
            );
            arena.set_paint_recording_in_progress(false);
            output
        };
        let mut paint_state = arena.paint_state().borrow_mut();
        paint_state.hit_test_list_generation += 1;
        debug_assert_eq!(output.hit_test_list.generation, paint_state.hit_test_list_generation);
        if paint_state
            .hit_test_item_cache_source
            .as_ref()
            .is_some_and(|source| source.visual_context_tree_version != output.compatible_visual_context_tree_version)
        {
            paint_state.hit_test_item_cache_source = None;
        }
        let list = std::mem::take(&mut output.hit_test_list);
        if inputs.paint_command_cache_read_write {
            paint_state.hit_test_item_cache_source = Some(std::rc::Rc::new(
                crate::painting::record::cache::HitTestItemCacheSource {
                    id: list.generation,
                    visual_context_tree_version: output.compatible_visual_context_tree_version,
                    items: list.items.clone(),
                },
            ));
        }
        paint_state.hit_test_list = Some(list);
        let output = std::rc::Rc::new(output);
        if inputs.paint_command_cache_read_write {
            paint_state.paint_command_cache_source = Some(output.clone());
            // Read-only recordings commit nothing and must not age dirty stamps out.
            arena.note_paint_record_completed_with_cache_writes();
        }
        paint_state.last_recording = Some(output);
        list_generation_of(&paint_state)
    })
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
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by
        // FfiPaintHostCallbacks::selection_style_facts.
        let shadows = unsafe { &mut *sink.cast::<Vec<crate::painting::record::paint::text::ShadowLayer>>() };
        shadows.push(crate::painting::record::paint::text::ShadowLayer {
            color: color.0,
            offset_x,
            offset_y,
            blur_radius,
        });
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
    abort_on_panic(|| {
        // SAFETY: `sink` is the ColorStopSink pointer handed out by FfiPaintHostCallbacks::background_layer_image.
        let sink = unsafe { &mut *sink.cast::<crate::painting::host::ColorStopSink>() };
        sink.colors.push(color);
        sink.positions.push(position);
    });
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiImagePaintRecordKind {
    DecodedFrame,
    NestedDisplayList,
    LinearGradient,
    RadialGradient,
    ConicGradient,
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
    pub gradient_angle: f32,
    pub first_stop_position: f32,
    pub repeat_length: f32,
    pub interpolation_method: libgfx_rust::GradientInterpolationMethod,
    pub center: used_values::FfiCssPixelPoint,
    pub size: used_values::FfiCssPixelSize,
    pub position: used_values::FfiCssPixelPoint,
    pub color_stop_colors: *const libgfx_rust::Color,
    pub color_stop_positions: *const f32,
    pub color_stop_count: usize,
    pub color_stops_repeating: bool,
}

/// # Safety
///
/// `inputs` and its arrays must be live for the call; `consume` is called synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_record_image_paint_display_list(
    inputs: *const FfiImagePaintRecordInputs,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, FfiRecordedDisplayList),
) {
    use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId};
    use crate::painting::display_list::recorder::{
        ColorStops, ConicGradientData, DisplayListRecorder, LinearGradientData, RadialGradientData,
    };
    use libgfx_rust::{CompositingAndBlendingOperator, IntRect};
    abort_on_panic(|| {
        let inputs = unsafe { &*inputs };
        let mut recorder = DisplayListRecorder::new(Vec::new());
        let dest_rect = inputs.dest_rect;
        let dest_int_rect = IntRect::new(
            inputs.dest_rect.x as i32,
            inputs.dest_rect.y as i32,
            inputs.dest_rect.width as i32,
            inputs.dest_rect.height as i32,
        );
        let color_stops = || {
            let count = inputs.color_stop_count;
            let (colors, positions) = if count == 0 {
                (Vec::new(), Vec::new())
            } else {
                // SAFETY: the host borrows the stop arrays for the duration of the call.
                unsafe {
                    (
                        std::slice::from_raw_parts(inputs.color_stop_colors, count).to_vec(),
                        std::slice::from_raw_parts(inputs.color_stop_positions, count).to_vec(),
                    )
                }
            };
            ColorStops {
                colors,
                positions,
                repeating: inputs.color_stops_repeating,
            }
        };
        let interpolation_method = inputs.interpolation_method;
        match inputs.kind {
            FfiImagePaintRecordKind::DecodedFrame => recorder.draw_scaled_decoded_image_frame(
                dest_rect,
                None,
                ImageFrameResourceId(inputs.frame_id),
                inputs.scaling_mode,
                CompositingAndBlendingOperator::Normal,
                None,
            ),
            FfiImagePaintRecordKind::NestedDisplayList => recorder.paint_nested_display_list(
                DisplayListResourceId(inputs.nested_display_list_id),
                dest_rect,
                inputs.nested_display_list_size,
            ),
            FfiImagePaintRecordKind::LinearGradient => recorder.fill_rect_with_linear_gradient(
                dest_int_rect,
                &LinearGradientData {
                    gradient_angle: inputs.gradient_angle,
                    color_stops: color_stops(),
                    first_stop_position: inputs.first_stop_position,
                    repeat_length: inputs.repeat_length,
                    interpolation_method,
                },
            ),
            FfiImagePaintRecordKind::RadialGradient => {
                let converter = crate::painting::display_list::device_pixels::DevicePixelConverter::new(
                    inputs.device_pixels_per_css_pixel,
                );
                recorder.fill_rect_with_radial_gradient(
                    dest_int_rect,
                    &RadialGradientData {
                        color_stops: color_stops(),
                        interpolation_method,
                    },
                    converter.rounded_device_point(inputs.center.into()),
                    converter.rounded_device_size(inputs.size.into()),
                );
            }
            FfiImagePaintRecordKind::ConicGradient => {
                let converter = crate::painting::display_list::device_pixels::DevicePixelConverter::new(
                    inputs.device_pixels_per_css_pixel,
                );
                recorder.fill_rect_with_conic_gradient(
                    dest_int_rect,
                    &ConicGradientData {
                        start_angle: inputs.gradient_angle,
                        color_stops: color_stops(),
                        interpolation_method,
                    },
                    converter.rounded_device_point(inputs.position.into()),
                );
            }
        }
        let recorded = recorder.into_builder().finish();
        unsafe { consume(context, (&recorded).into()) };
    });
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_overlay_glyph(sink: *mut c_void, glyph_id: u32, x: f32, y: f32) {
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by FfiPaintHostCallbacks::overlay_label.
        let glyphs = unsafe { &mut *sink.cast::<Vec<crate::painting::display_list::commands::DisplayListGlyph>>() };
        glyphs.push(crate::painting::display_list::commands::DisplayListGlyph {
            position: libgfx_rust::FloatPoint { x, y },
            glyph_id,
        });
    });
}

/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_spatial_node_count(tree: *const c_void) -> usize {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        tree.spatial_nodes.len()
    })
}

/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_frame_node_count(tree: *const c_void) -> usize {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        tree.frame_nodes.len()
    })
}

/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_root_is_visual_viewport(tree: *const c_void) -> bool {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        tree.root_is_visual_viewport
    })
}

///
/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_root_isolation_frame(tree: *const c_void) -> u32 {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        tree.root_isolation_frame
            .unwrap_or(crate::painting::visual_context::FrameNodeIndex::NONE)
            .0
    })
}

fn export_visual_context_nodes<Node>(
    nodes: &[Node],
    begin: usize,
    out: *mut crate::painting::host::FfiVisualContextNodeExport,
    capacity: usize,
    export: fn(&Node) -> crate::painting::host::FfiVisualContextNodeExport,
) {
    let nodes = &nodes[begin..begin + capacity];
    // SAFETY: The caller provides writable storage for `capacity` exports.
    let out = unsafe { std::slice::from_raw_parts_mut(out, capacity) };
    for (out, node) in out.iter_mut().zip(nodes) {
        *out = export(node);
    }
}

/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously; `out` must have room
/// for `capacity` exports, and `begin..begin + capacity` must lie within the spatial nodes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_export_spatial_nodes(
    tree: *const c_void,
    begin: usize,
    out: *mut crate::painting::host::FfiVisualContextNodeExport,
    capacity: usize,
) {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        export_visual_context_nodes(
            &tree.spatial_nodes,
            begin,
            out,
            capacity,
            crate::painting::visual_context::export_spatial_node,
        );
    });
}

/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously; `out` must have room
/// for `capacity` exports, and `begin..begin + capacity` must lie within the frame nodes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_export_frame_nodes(
    tree: *const c_void,
    begin: usize,
    out: *mut crate::painting::host::FfiVisualContextNodeExport,
    capacity: usize,
) {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        export_visual_context_nodes(
            &tree.frame_nodes,
            begin,
            out,
            capacity,
            crate::painting::visual_context::export_frame_node,
        );
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_spliced_capture_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .last_recording
            .as_ref()
            .map_or(0, |recording| recording.spliced_capture_count)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_has_blocking_wheel_event_listeners(arena: *mut c_void) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .last_recording
            .as_ref()
            .is_some_and(|recording| recording.has_blocking_wheel_event_listeners)
    })
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if propagated_text_decorations {
            arena.invalidate_propagated_text_decoration_caches(paintable);
        } else {
            arena.invalidate_paint_cache(paintable);
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_invalidate_for_repaint(arena: *mut c_void, paintable: NodeSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.invalidate_for_repaint(paintable);
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_invalidate_all_paint_caches(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.mark_all_paint_caches_dirty();
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_computed_svg_path(
    arena: *mut c_void,
    paintable: NodeSlotId,
) -> *const c_void {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(paintable) {
            return std::ptr::null();
        }
        crate::painting::paintable_geometry::committed_svg_path(&paintable_rows, paintable)
            .map_or(std::ptr::null(), |path| path.as_raw())
    })
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
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_caret_rect_for_position(
    arena: *mut c_void,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    offset: usize,
    affinity_is_downstream: bool,
) -> FfiCaretRectResult {
    abort_on_panic(|| {
        let mut result = FfiCaretRectResult {
            found: false,
            rect: FfiCssPixelRect::default(),
            style_source: std::ptr::null_mut(),
            owner_paintable: NodeSlotId::INVALID,
            nearest_self_painting_inline: NodeSlotId::INVALID,
        };
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        let Some(answer) = crate::painting::caret::caret_rect_for_position(
            &paintable_rows,
            node_slots,
            offset,
            affinity_is_downstream,
        ) else {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_caret_rect_in_dom_range(
    arena: *mut c_void,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    offset: usize,
) -> FfiOptionalCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
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
    })
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
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_empty_line_caret_rect(
    arena: *mut c_void,
    block: NodeSlotId,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    offset: usize,
) -> FfiEmptyLineCaretRect {
    abort_on_panic(|| {
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
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        let side = arena.paintable_side_data(block);
        let Some(first_fragment) = side.fragments.first() else {
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
    })
}

fn with_inline_pieces(
    arena: &impl PaintableRowsRead,
    inline_paintable: NodeSlotId,
    mut callback: impl FnMut(&InlineBoxPieceRecord, &PaintableData) -> bool,
) {
    let Some(root) = arena.inline_pieces_root(inline_paintable) else {
        return;
    };
    let data = arena.paintable_data(inline_paintable);
    let root_side = arena.paintable_side_data(root);
    for piece_index in &arena.paintable_side_data(inline_paintable).piece_indices {
        let piece = &root_side.inline_box_pieces[*piece_index as usize];
        if !callback(piece, data) {
            return;
        }
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_inline_paintable_piece_border_box_rects(
    arena: *mut c_void,
    inline_paintable: NodeSlotId,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        let Some(root) = paintable_rows.inline_pieces_root(inline_paintable) else {
            return;
        };
        let root_position = crate::painting::paintable_geometry::absolute_position(&paintable_rows, root);
        with_inline_pieces(&paintable_rows, inline_paintable, |piece, _| {
            if piece.is_geometry_only_placeholder {
                return true;
            }
            let rect = crate::css::css_pixels::CssPixelRect::from(piece.border_box_rect).translated_by(root_position);
            // SAFETY: The consumer copies the plain-data rect synchronously.
            unsafe { push_rect(context, rect.into()) };
            true
        });
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_inline_paintable_has_content_pieces(
    arena: *mut c_void,
    inline_paintable: NodeSlotId,
) -> bool {
    abort_on_panic(|| {
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
    })
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
    abort_on_panic(|| {
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
            let border_rect = crate::css::css_pixels::CssPixelRect::from(piece.border_box_rect);
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
    })
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
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_visual_lines(
    arena: *mut c_void,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, FfiVisualLine),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
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
    });
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
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_has_rendered_text_before(
    arena: *mut c_void,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    offset: usize,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
            fragment.dom_start_offset_in_node < offset
        })
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_has_rendered_text_after(
    arena: *mut c_void,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    offset: usize,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
            fragment.dom_end_offset_in_node > offset
        })
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
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_line_caret_inline_coordinate(
    arena: *mut c_void,
    owner_paintable: u32,
    line_index: u32,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    offset: usize,
) -> FfiOptionalCssPixels {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_line_offset_closest_to_inline_coordinate(
    arena: *mut c_void,
    owner_paintable: u32,
    line_index: u32,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    inline_coordinate: CssPixels,
    fallback_offset: usize,
) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        crate::painting::visual_lines::offset_closest_to_inline_coordinate(
            &paintable_rows,
            owner_paintable,
            line_index,
            node_slots,
            inline_coordinate,
        )
        .unwrap_or(fallback_offset)
    })
}

#[repr(C)]
pub struct FfiFragmentTextFacts {
    pub layout_node: *mut c_void,
    pub dom_start_offset_in_node: usize,
    pub dom_end_offset_in_node: usize,
    pub dom_end_offset_with_trailing_whitespace: usize,
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_fragment_text_facts(
    arena: *mut c_void,
    block: NodeSlotId,
    fragment_index: u32,
) -> FfiFragmentTextFacts {
    abort_on_panic(|| {
        let mut facts = FfiFragmentTextFacts {
            layout_node: std::ptr::null_mut(),
            dom_start_offset_in_node: 0,
            dom_end_offset_in_node: 0,
            dom_end_offset_with_trailing_whitespace: 0,
        };
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(block) {
            return facts;
        }
        let side_data = arena.paintable_side_data(block);
        let Some(fragment) = side_data.fragments.get(fragment_index as usize) else {
            return facts;
        };
        facts.layout_node = arena.shell_if_live(fragment.layout_node);
        facts.dom_start_offset_in_node = fragment.dom_start_offset_in_node;
        facts.dom_end_offset_in_node = fragment.dom_end_offset_in_node;
        facts.dom_end_offset_with_trailing_whitespace = fragment.dom_end_offset_with_trailing_whitespace;
        facts
    })
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiCaretMatch {
    None,
    Direct,
    SoftWrapFallback,
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_fragment_caret_match(
    arena: *mut c_void,
    block: NodeSlotId,
    fragment_index: u32,
    offset: usize,
    affinity_is_downstream: bool,
) -> FfiCaretMatch {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(block) {
            return FfiCaretMatch::None;
        }
        let side_data = arena.paintable_side_data(block);
        let Some(fragment) = side_data.fragments.get(fragment_index as usize) else {
            return FfiCaretMatch::None;
        };
        match crate::painting::text_fragment::caret_match(fragment, offset, affinity_is_downstream) {
            crate::painting::text_fragment::CaretMatch::None => FfiCaretMatch::None,
            crate::painting::text_fragment::CaretMatch::Direct => FfiCaretMatch::Direct,
            crate::painting::text_fragment::CaretMatch::SoftWrapFallback => FfiCaretMatch::SoftWrapFallback,
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_fragment_index_in_node_for_point(
    arena: *mut c_void,
    block: NodeSlotId,
    fragment_index: u32,
    x: CssPixels,
    y: CssPixels,
) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(block) {
            return 0;
        }
        let side_data = arena.paintable_side_data(block);
        let Some(fragment) = side_data.fragments.get(fragment_index as usize) else {
            return 0;
        };
        crate::painting::text_fragment::index_in_node_for_point(
            &paintable_rows,
            fragment,
            crate::css::css_pixels::CssPixelPoint { x, y },
        )
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_fragment_caret_range_rect(
    arena: *mut c_void,
    block: NodeSlotId,
    fragment_index: u32,
    offset: usize,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(block) {
            return FfiCssPixelRect::default();
        }
        let side_data = arena.paintable_side_data(block);
        let Some(fragment) = side_data.fragments.get(fragment_index as usize) else {
            return FfiCssPixelRect::default();
        };
        let offsets = crate::painting::text_fragment::compute_selection_offsets(
            &paintable_rows,
            fragment,
            SELECTION_STATE_START_AND_END,
            offset,
            offset,
        );
        match offsets {
            Some(offsets) => {
                crate::painting::text_fragment::rect_for_selection_offsets(&paintable_rows, fragment, offsets, || {
                    crate::painting::text_fragment::first_available_font(&paintable_rows, fragment)
                })
                .into()
            }
            None => FfiCssPixelRect::default(),
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread. `node_slots` must point at `node_slot_count` valid slots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_range_rects(
    arena: *mut c_void,
    node_slots: *const NodeSlotId,
    node_slot_count: usize,
    selection_state: u8,
    range_start_offset: usize,
    range_end_offset: usize,
    filter_dom_start: usize,
    filter_dom_end: usize,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiCssPixelRect),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();

        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        crate::painting::text_fragment::for_each_fragment_of_nodes(
            &paintable_rows,
            node_slots,
            |block, _, fragment| {
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

                // SAFETY: The consumer handles a null shell and copies the rect synchronously.
                unsafe { push_rect(context, arena.shell_if_live(block), rect.into()) };
                true
            },
        );
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
    abort_on_panic(|| {
        let mut result = FfiOptionalCssPixelRect {
            has_value: false,
            rect: FfiCssPixelRect::default(),
        };
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(block) {
            return result;
        }
        for fragment in &arena.paintable_side_data(block).fragments {
            if fragment.layout_node != node {
                continue;
            }
            result.has_value = true;
            result.rect = crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into();
            break;
        }
        result
    })
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        if !paintable_rows.paintable_row_is_populated(root) {
            return;
        }
        crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, root, |current| {
            for fragment in &arena.paintable_side_data(current).fragments {
                let shell = arena.shell_if_live(fragment.layout_node);
                let rect = crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into();
                // SAFETY: The consumer copies its plain-data arguments synchronously.
                unsafe { consume(context, shell, rect) };
            }
        });
    });
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
    abort_on_panic(|| {
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
    });
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
    abort_on_panic(|| {
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
    });
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(paintable) {
            return;
        }
        if let Some(data) = crate::painting::paintable_geometry::committed_grid_layout_data(arena, paintable) {
            let json = crate::painting::devtools_layout::serialize_grid_layout(&data, container_node_id);
            unsafe { consume(context, json.as_ptr(), json.len()) };
        }
    });
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(paintable) {
            return;
        }
        if let Some(data) = crate::painting::paintable_geometry::committed_flex_layout_data(arena, paintable) {
            let json = crate::painting::devtools_layout::serialize_flex_layout(&data, container_node_id);
            unsafe { consume(context, json.as_ptr(), json.len()) };
        }
    });
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
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        if !arena.paintable_row_is_populated(paintable) {
            return;
        }
        if let Some(tracks) = crate::painting::paintable_geometry::committed_used_grid_tracks(arena, paintable) {
            tracks.with_ffi_views(|columns, rows| unsafe { consume(context, columns, rows) });
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_stacking_context_tree_node_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .stacking_context_tree
            .as_ref()
            .map_or(0, |tree| tree.nodes.len())
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_stacking_context_tree_node(
    arena: *mut c_void,
    index: usize,
) -> crate::painting::host::FfiStackingContextNodeExport {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        let node = &paint_state
            .stacking_context_tree
            .as_ref()
            .expect("no stacking context tree")
            .nodes[index];
        crate::painting::host::FfiStackingContextNodeExport {
            layout_node_shell: if arena.paintable_row_is_populated(node.paintable) {
                arena.shell_if_live(node.paintable)
            } else {
                std::ptr::null_mut()
            },
            child_count: node.children.len(),
            has_effective_z_index: node.effective_z_index.is_some(),
            effective_z_index: node.effective_z_index.unwrap_or(0),
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; indices in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_stacking_context_tree_child(
    arena: *mut c_void,
    index: usize,
    child: usize,
) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .stacking_context_tree
            .as_ref()
            .expect("no stacking context tree")
            .nodes[index]
            .children[child] as usize
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_main_visual_context_tree(arena: *mut c_void) -> *const c_void {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .visual_context
            .tree
            .as_ref()
            .map_or(std::ptr::null(), |tree| std::ptr::from_ref(tree).cast())
    })
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
    abort_on_panic(|| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state.hit_test_list.as_ref().map_or(0, |list| list.items.len())
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `output` must point to writable
/// storage for exactly `output_length` items, matching the current hit-test item count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_export_hit_test_items(
    arena: *mut c_void,
    output: *mut crate::painting::host::FfiHitTestItemExport,
    output_length: usize,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        let items = &paint_state.hit_test_list.as_ref().expect("no hit-test list").items;
        assert_eq!(items.len(), output_length);
        if items.is_empty() {
            return;
        }
        assert!(!output.is_null());
        // SAFETY: The caller provides writable storage for exactly `output_length` exports.
        let output = unsafe { std::slice::from_raw_parts_mut(output, output_length) };
        for (output, item) in output.iter_mut().zip(items.iter()) {
            let rect = |rect: crate::css::css_pixels::CssPixelRect| used_values::FfiCssPixelRect::from(rect);
            assert!(
                arena.paintable_row_is_populated(item.paintable),
                "exporting a hit-test item for a non-live paintable"
            );
            *output = crate::painting::host::FfiHitTestItemExport {
                kind: item.kind as u8,
                paintable: item.paintable,
                chrome_widget_kind: item.chrome_widget_kind,
                text_fragment_index: item.text_fragment_index.into(),
                caret_node_shell: arena.shell_if_live(item.caret_node),
                caret_offset: item.caret_offset,
                rect: rect(item.rect),
                caret_rect: rect(item.caret_rect),
                context: item.context,
            };
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_display_list_mask_registration_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .last_recording
            .as_ref()
            .map_or(0, |recording| recording.mask_display_lists.len())
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_display_list_mask_registration(
    arena: *mut c_void,
    index: usize,
    out_frame: *mut crate::painting::display_list::commands::FrameNodeIndex,
    out_display_list_id: *mut u64,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        let (frame, id) = paint_state
            .last_recording
            .as_ref()
            .expect("no recording")
            .mask_display_lists[index];
        // SAFETY: the caller passes valid out pointers.
        unsafe {
            *out_frame = frame;
            *out_display_list_id = id.0;
        }
    });
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously; `bytes` must point at
/// `length` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_bytes(sink: *mut c_void, bytes: *const u8, length: usize) {
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by the host callback wrapper; the caller
        // guarantees the byte range.
        let vec = unsafe { &mut *sink.cast::<Vec<u8>>() };
        if length > 0 {
            vec.extend_from_slice(unsafe { std::slice::from_raw_parts(bytes, length) });
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// returned pointers borrow the last recording and stay valid until the next one replaces it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_recorded_display_list(arena: *mut c_void) -> FfiRecordedDisplayList {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paint_state = arena.paint_state().borrow();
        paint_state
            .last_recording
            .as_ref()
            .map_or_else(FfiRecordedDisplayList::empty, |recording| {
                (&recording.display_list).into()
            })
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_line_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| with_hit_test_list(arena, 0, |list, _| list.caret_lines.len()))
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `line_index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_line(
    arena: *mut c_void,
    line_index: usize,
) -> crate::painting::host::FfiCaretLineExport {
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, _| {
            let line = &list.caret_lines[line_index];
            crate::painting::host::FfiCaretLineExport {
                rect: line.rect.into(),
                context: line.context,
                first_caret_item_index: line.first_caret_item_index,
                last_caret_item_index: line.last_caret_item_index,
            }
        })
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_item_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| with_hit_test_list(arena, 0, |list, _| list.caret_item_indices.len()))
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `caret_item_index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_item_index(arena: *mut c_void, caret_item_index: usize) -> usize {
    abort_on_panic(|| with_hit_test_list(arena, usize::MAX, |list, _| list.caret_item_indices[caret_item_index]))
}

fn list_generation_of(paint_state: &crate::painting::paint_state::PaintState) -> u64 {
    paint_state.hit_test_list.as_ref().map_or(0, |list| list.generation)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_list_generation(arena: *mut c_void) -> u64 {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        list_generation_of(&arena.paint_state().borrow())
    })
}

fn with_hit_test_list<R>(
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
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, arena| {
            ffi_topmost(list.find_topmost_item(arena, &callbacks, point.into()))
        })
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
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, arena| {
            let (caret_item, hit_item) = list.find_topmost_items_for_caret(arena, &callbacks, point.into());
            crate::painting::host::FfiTopmostItemsForCaret {
                caret_item: ffi_topmost(caret_item),
                hit_item: ffi_topmost(hit_item),
            }
        })
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
    abort_on_panic(|| {
        let indices = with_hit_test_list(arena, Vec::new(), |list, arena| {
            list.hit_test_all(arena, &callbacks, point.into())
        });
        for index in indices {
            // SAFETY: The C++ sink consumes the index synchronously.
            unsafe { push(push_context, index) };
        }
    });
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
    abort_on_panic(|| {
        let position_type = crate::painting::hit_test::caret::CaretPositionType::from_u8(position_type);
        with_hit_test_list(arena, usize::MAX, |list, _| {
            list.item_at_line_edge(line_index, position_type)
        })
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
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, _| {
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
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_line_block_coordinate(arena: *mut c_void, line_index: usize) -> i32 {
    abort_on_panic(|| with_hit_test_list(arena, 0, |list, _| list.line_block_coordinate(line_index).raw_value()))
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
    abort_on_panic(|| {
        with_hit_test_list(arena, false, |list, _| {
            list.item_is_inline_adjacent_to_line(item_index, line_index)
        })
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
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, _| {
            let closest = list.find_closest_line(
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
    abort_on_panic(|| {
        let direction = if direction == 1 {
            crate::painting::hit_test::caret::CaretLineDirection::Next
        } else {
            crate::painting::hit_test::caret::CaretLineDirection::Previous
        };
        with_hit_test_list(arena, Default::default(), |list, _| {
            match list.adjacent_line(
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
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by FfiHitTestHostCallbacks::line_break_caret_targets.
        let targets = unsafe { &mut *sink.cast::<Vec<crate::painting::host::FfiLineBreakCaretTarget>>() };
        targets.push(target);
    });
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiResolvedGradientPaintKind {
    #[default]
    None,
    Linear,
    Radial,
    Conic,
}

#[repr(C)]
pub struct FfiResolvedGradientPaint {
    pub kind: FfiResolvedGradientPaintKind,
    pub gradient_angle: f32,
    pub first_stop_position: f32,
    pub repeat_length: f32,
    pub color_stops_repeating: bool,
    pub interpolation_method: libgfx_rust::GradientInterpolationMethod,
    pub center: FfiCssPixelPoint,
    pub size: used_values::FfiCssPixelSize,
}

/// # Safety
///
/// `gradient_value` must point at a live gradient `StyleValueData` and
/// `current_color_value` at a live `StyleValueData` or null, both borrowed
/// for this call. `out` must point at writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_resolve_gradient_paint_for_size(
    gradient_value: *const c_void,
    current_color: libgfx_rust::Color,
    current_color_value: *const c_void,
    color_scheme: u8,
    size: FfiCssPixelSize,
    out: *mut FfiResolvedGradientPaint,
    stop_context: *mut c_void,
    append_stop: unsafe extern "C" fn(*mut c_void, libgfx_rust::Color, f32),
) {
    abort_on_panic(|| {
        use crate::painting::record::paint::gradient_resolution::{
            ResolvedGradientPaint, resolve_gradient_paint_with_input,
        };
        let gradient_value = unsafe { &*gradient_value.cast::<crate::css::style_value::StyleValueData>() };
        let current_color_value = unsafe {
            current_color_value
                .cast::<crate::css::style_value::StyleValueData>()
                .as_ref()
        };
        let color_input = crate::css::color_resolution::ColorResolutionInput {
            scheme: Some(color_scheme),
            current_color: Some(crate::css::color_resolution::Rgba {
                r: current_color.red(),
                g: current_color.green(),
                b: current_color.blue(),
                a: current_color.alpha(),
            }),
            current_color_value,
            length: None,
            channels: None,
        };
        let tile_size = size.into();
        let resolved = resolve_gradient_paint_with_input(gradient_value, tile_size, &color_input);
        let out = unsafe { &mut *out };
        let (data_stops, interpolation_method) = match &resolved {
            ResolvedGradientPaint::Linear(data) => {
                out.kind = FfiResolvedGradientPaintKind::Linear;
                out.gradient_angle = data.gradient_angle;
                out.first_stop_position = data.first_stop_position;
                out.repeat_length = data.repeat_length;
                (&data.color_stops, data.interpolation_method)
            }
            ResolvedGradientPaint::Radial { data, center, size } => {
                out.kind = FfiResolvedGradientPaintKind::Radial;
                out.center = FfiCssPixelPoint {
                    x: center.x,
                    y: center.y,
                };
                out.size = used_values::FfiCssPixelSize {
                    width: size.width,
                    height: size.height,
                };
                (&data.color_stops, data.interpolation_method)
            }
            ResolvedGradientPaint::Conic { data, position } => {
                out.kind = FfiResolvedGradientPaintKind::Conic;
                out.gradient_angle = data.start_angle;
                out.center = FfiCssPixelPoint {
                    x: position.x,
                    y: position.y,
                };
                (&data.color_stops, data.interpolation_method)
            }
        };
        out.color_stops_repeating = data_stops.repeating;
        out.interpolation_method = interpolation_method;
        for (color, position) in data_stops.colors.iter().zip(data_stops.positions.iter()) {
            // SAFETY: The C++ caller appends into its own storage synchronously.
            unsafe { append_stop(stop_context, *color, *position) };
        }
    });
}
