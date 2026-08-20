/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::css::css_pixels::CssPixels;
use crate::layout::FfiCssPixelPoint;
use crate::layout::FfiCssPixelRect;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::*;
use std::ffi::c_void;

/// SAFETY: `arena` must be a live handle from `layout_arena_create`, borrowed for this call on
/// the document thread.
unsafe fn arena_from_handle<'a>(arena: *mut c_void) -> &'a LayoutNodeArena {
    unsafe { LayoutNodeArena::from_handle(arena) }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_row_for_node(
    arena: *mut c_void,
    layout_node: NodeSlotId,
    shell: *mut c_void,
) -> PaintableAllocation {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintables().borrow_mut().row_for_node(layout_node, shell)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_shell_destroyed(
    arena: *mut c_void,
    slot: PaintableSlotId,
    generation: u32,
    shell: *mut c_void,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintables().borrow_mut().shell_destroyed(slot, generation, shell);
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_cleared_from_node(
    arena: *mut c_void,
    layout_node: NodeSlotId,
    slot: PaintableSlotId,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintables().borrow_mut().node_cleared(layout_node, slot);
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_transfer_fragments_to_replacement_node(
    arena: *mut c_void,
    containing_block: PaintableSlotId,
    old_node: NodeSlotId,
    new_node: NodeSlotId,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        if !paintables.is_live(containing_block) {
            return;
        }
        let side = paintables.side_mut(containing_block);
        for fragment in &mut side.fragments {
            if fragment.layout_node == old_node {
                fragment.layout_node = new_node;
            }
        }
        for piece in &mut side.inline_box_pieces {
            if piece.node == old_node {
                piece.node = new_node;
            }
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_shell(arena: *mut c_void, slot: PaintableSlotId) -> *mut c_void {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(slot) {
            return std::ptr::null_mut();
        }
        paintables.data_ref(slot).shell
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
    viewport: PaintableSlotId,
    entries: *const FfiSelectionEntry,
    entry_count: usize,
    range_start_offset: usize,
    range_end_offset: usize,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        if !paintables.is_live(viewport) {
            return;
        }
        // SAFETY: The caller guarantees the entry span is valid for this synchronous call.
        let entries = unsafe { ffi_slice(entries, entry_count) };
        crate::painting::selection::apply(
            arena,
            &mut paintables,
            viewport,
            entries,
            crate::painting::selection::SelectionRange {
                start_offset: range_start_offset,
                end_offset: range_end_offset,
            },
        );
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_selection_clear(arena: *mut c_void, viewport: PaintableSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        if !paintables.is_live(viewport) {
            return;
        }
        crate::painting::selection::clear(arena, &mut paintables, viewport);
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_set_sticky_insets(
    arena: *mut c_void,
    slot: PaintableSlotId,
    insets: FfiStickyInsets,
    has_insets: bool,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| {
                data.sticky_insets = insets;
                data.has_sticky_insets = has_insets;
            });
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_clear_overflow_data(arena: *mut c_void, slot: PaintableSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| {
                data.overflow = FfiOverflowData::default();
                data.has_overflow = false;
            });
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_clear_cached_overflow_data(arena: *mut c_void, slot: PaintableSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| {
                data.cached_overflow = FfiOverflowData::default();
                data.has_cached_overflow = false;
            });
        }
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
    box_paintable: PaintableSlotId,
    visual_context_callbacks: crate::painting::host::FfiVisualContextHostCallbacks,
    overflow_callbacks: crate::painting::host::FfiScrollableOverflowHostCallbacks,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(box_paintable) {
            return;
        }
        crate::painting::scrollable_overflow::measure_scrollable_overflow(
            arena,
            &paintables,
            &visual_context_callbacks,
            &overflow_callbacks,
            box_paintable,
        );
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_rect(
    arena: *mut c_void,
    slot: PaintableSlotId,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_rect(&paintables, slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_padding_box_rect(
    arena: *mut c_void,
    slot: PaintableSlotId,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_padding_box_rect(&paintables, slot).into()
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_border_box_rect(
    arena: *mut c_void,
    slot: PaintableSlotId,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_border_box_rect(&paintables, slot).into()
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
        let mut contained_boxes_by_containing_block =
            std::mem::take(&mut arena.paintables().borrow_mut().scrollable_overflow_contained_boxes);
        {
            let paintables = arena.paintables().borrow();
            crate::painting::scrollable_overflow::refill_contained_boxes_index(
                arena,
                &paintables,
                root,
                &mut contained_boxes_by_containing_block,
            );
        }
        arena.paintables().borrow_mut().scrollable_overflow_contained_boxes = contained_boxes_by_containing_block;
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
            .paintables()
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
    paintable: PaintableSlotId,
) -> FfiPhysicalOverflowDirections {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let directions = if paintables.is_live(paintable) {
            crate::painting::scrollable_overflow::physical_overflow_directions(
                arena,
                paintables.data_ref(paintable).layout_node,
            )
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
pub unsafe extern "C" fn layout_arena_build_stacking_context_tree(arena: *mut c_void, root: PaintableSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let tree = {
            let paintables = arena.paintables().borrow();
            if !paintables.is_live(root) {
                return;
            }
            crate::painting::stacking_context::build_stacking_context_tree(arena, &paintables, root)
        };
        let mut paintables = arena.paintables().borrow_mut();
        paintables.stacking_context_tree = Some(tree);
        crate::painting::fragment_ownership::assign_fragment_ownership(arena, &mut paintables, root);
    });
}

use crate::painting::host::FfiVisualContextHostCallbacks;

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_assign_accumulated_visual_contexts(
    arena: *mut c_void,
    viewport: PaintableSlotId,
    callbacks: FfiVisualContextHostCallbacks,
    force_incompatible_rebuild: bool,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let (mut tree, scroll_state, paintables_with_mask_nodes, previous_assignments) = {
            let paintables = arena.paintables().borrow();
            if !paintables.is_live(viewport) {
                return false;
            }
            let previous_assignments = paintables.visual_context_assignments();
            let (tree, scroll_state, paintables_with_mask_nodes) =
                crate::painting::visual_context::build::build_visual_context_tree(
                    arena,
                    &paintables,
                    &callbacks,
                    viewport,
                );
            (tree, scroll_state, paintables_with_mask_nodes, previous_assignments)
        };
        let mut paintables = arena.paintables().borrow_mut();
        let assignments_changed = previous_assignments != paintables.visual_context_assignments();
        let state = &mut paintables.visual_context;
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
            paintables.clear_descendant_subtree_caches();
        }
        is_compatible
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_visual_context_values(
    arena: *mut c_void,
    paintable: PaintableSlotId,
    callbacks: FfiVisualContextHostCallbacks,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        if !paintables.is_live(paintable) {
            return false;
        }
        let pixel_ratio = callbacks.tree_inputs().device_pixels_per_css_pixel;
        let Some(mut tree) = paintables.visual_context.tree.take() else {
            return false;
        };
        let updated = crate::painting::visual_context::build::update_visual_context_values(
            arena,
            &paintables,
            &callbacks,
            &mut tree,
            paintable,
            pixel_ratio,
        );
        paintables.visual_context.tree = Some(tree);
        updated
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
        let mut paintables = arena.paintables().borrow_mut();
        let Some(tree) = &mut paintables.visual_context.tree else {
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
            .paintables()
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
        let mut paintables = arena.paintables().borrow_mut();
        let state = &mut paintables.visual_context;
        state.scroll_state.clear();
        state.scroll_state_snapshot.clear();
        state.needs_to_refresh_scroll_state = true;
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_sticky_constraints(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        let mut scroll_state = std::mem::take(&mut paintables.visual_context.scroll_state);
        crate::painting::visual_context::refresh::refresh_sticky_constraints(&paintables, &mut scroll_state);
        paintables.visual_context.scroll_state = scroll_state;
        paintables.visual_context.needs_to_refresh_scroll_state = true;
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
        let mut paintables = arena.paintables().borrow_mut();
        if !paintables.visual_context.needs_to_refresh_scroll_state {
            return;
        }
        paintables.visual_context.needs_to_refresh_scroll_state = false;
        let mut scroll_state = std::mem::take(&mut paintables.visual_context.scroll_state);
        crate::painting::visual_context::refresh::refresh_scroll_state(&paintables, &callbacks, &mut scroll_state);
        let snapshot = scroll_state.snapshot(callbacks.tree_inputs().device_pixels_per_css_pixel);
        paintables.visual_context.scroll_state = scroll_state;
        paintables.visual_context.scroll_state_snapshot = snapshot;
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reset_visual_context_state(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        let next_tree_version = paintables.visual_context.next_tree_version;
        paintables.visual_context = crate::painting::visual_context::VisualContextState {
            needs_to_refresh_scroll_state: true,
            next_tree_version,
            ..Default::default()
        };
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_record_display_list(
    arena: *mut c_void,
    viewport: PaintableSlotId,
    callbacks: crate::painting::host::FfiHitTestHostCallbacks,
    paint_callbacks: crate::painting::host::FfiPaintHostCallbacks,
    visual_context_callbacks: crate::painting::host::FfiVisualContextHostCallbacks,
    inputs: crate::painting::host::FfiRecordingInputs,
) -> u64 {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut output = {
            let paintables = arena.paintables().borrow();
            if !paintables.is_live(viewport) || paintables.stacking_context_tree.is_none() {
                return 0;
            }
            let command_cache_source = (!inputs.should_show_line_box_borders)
                .then(|| paintables.paint_command_cache_source.clone())
                .flatten();
            crate::painting::record::traversal::record_display_list(
                arena,
                &paintables,
                &callbacks,
                &paint_callbacks,
                &visual_context_callbacks,
                inputs,
                paintables.hit_test_list_generation + 1,
                command_cache_source,
                paintables.hit_test_item_cache_source.clone(),
            )
        };
        let mut paintables = arena.paintables().borrow_mut();
        paintables.hit_test_list_generation += 1;
        debug_assert_eq!(output.hit_test_list.generation, paintables.hit_test_list_generation);
        if paintables
            .hit_test_item_cache_source
            .as_ref()
            .is_some_and(|source| source.visual_context_tree_version != output.compatible_visual_context_tree_version)
        {
            paintables.hit_test_item_cache_source = None;
        }
        let list = std::mem::take(&mut output.hit_test_list);
        if inputs.paint_command_cache_read_write {
            paintables.hit_test_item_cache_source = Some(std::rc::Rc::new(
                crate::painting::record::cache::HitTestItemCacheSource {
                    id: list.generation,
                    visual_context_tree_version: output.compatible_visual_context_tree_version,
                    items: list.items.clone(),
                },
            ));
        }
        paintables.hit_test_list = Some(list);
        let output = std::rc::Rc::new(output);
        if inputs.paint_command_cache_read_write {
            paintables.paint_command_cache_source = Some(output.clone());
        }
        paintables.last_recording = Some(output);
        list_generation_of(&paintables)
    })
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_selection_shadow(
    sink: *mut c_void,
    color: u32,
    offset_x: CssPixels,
    offset_y: CssPixels,
    blur_radius: CssPixels,
) {
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by
        // FfiPaintHostCallbacks::selection_style_facts.
        let shadows = unsafe { &mut *sink.cast::<Vec<crate::painting::record::paint::text::ShadowLayer>>() };
        shadows.push(crate::painting::record::paint::text::ShadowLayer {
            color,
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
pub unsafe extern "C" fn layout_arena_paint_push_color_stop(sink: *mut c_void, color: u32, position: f32) {
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
    pub dest_rect: [f32; 4],
    pub frame_id: u64,
    pub scaling_mode: i32,
    pub nested_display_list_id: u64,
    pub nested_display_list_size: [i32; 2],
    pub gradient_angle: f32,
    pub first_stop_position: f32,
    pub repeat_length: f32,
    pub interpolation_type: u8,
    pub rectangular_color_space: u8,
    pub polar_color_space: u8,
    pub hue_interpolation_method: u8,
    pub center: [i32; 2],
    pub size: [i32; 2],
    pub position: [i32; 2],
    pub color_stop_colors: *const u32,
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
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId};
    use crate::painting::display_list::recorder::{
        ColorStops, ConicGradientData, DisplayListRecorder, LinearGradientData, RadialGradientData,
    };
    use libgfx_rust::{
        CompositingAndBlendingOperator, FloatRect, GradientInterpolationMethod, GradientInterpolationType,
        HueInterpolationMethod, IntPoint, IntRect, IntSize, PolarColorSpace, RectangularColorSpace, ScalingMode,
    };
    abort_on_panic(|| {
        let inputs = unsafe { &*inputs };
        let mut recorder = DisplayListRecorder::new(Vec::new());
        let dest_rect = FloatRect::new(
            inputs.dest_rect[0],
            inputs.dest_rect[1],
            inputs.dest_rect[2],
            inputs.dest_rect[3],
        );
        let dest_int_rect = IntRect::new(
            inputs.dest_rect[0] as i32,
            inputs.dest_rect[1] as i32,
            inputs.dest_rect[2] as i32,
            inputs.dest_rect[3] as i32,
        );
        let color_stops = || {
            let count = inputs.color_stop_count;
            let (colors, positions) = if count == 0 {
                (Vec::new(), Vec::new())
            } else {
                // SAFETY: the host borrows the stop arrays for the duration of the call.
                unsafe {
                    (
                        std::slice::from_raw_parts(inputs.color_stop_colors, count)
                            .iter()
                            .map(|value| libgfx_rust::Color(*value))
                            .collect(),
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
        let interpolation_method = GradientInterpolationMethod {
            interpolation_type: GradientInterpolationType::from_u8(inputs.interpolation_type),
            rectangular_color_space: RectangularColorSpace::from_u8(inputs.rectangular_color_space),
            polar_color_space: PolarColorSpace::from_u8(inputs.polar_color_space),
            hue_interpolation_method: HueInterpolationMethod::from_u8(inputs.hue_interpolation_method),
        };
        match inputs.kind {
            FfiImagePaintRecordKind::DecodedFrame => recorder.draw_scaled_decoded_image_frame(
                dest_rect,
                None,
                ImageFrameResourceId(inputs.frame_id),
                ScalingMode::from_raw(inputs.scaling_mode),
                CompositingAndBlendingOperator::Normal,
                None,
            ),
            FfiImagePaintRecordKind::NestedDisplayList => recorder.paint_nested_display_list(
                DisplayListResourceId(inputs.nested_display_list_id),
                dest_rect,
                IntSize {
                    width: inputs.nested_display_list_size[0],
                    height: inputs.nested_display_list_size[1],
                },
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
            FfiImagePaintRecordKind::RadialGradient => recorder.fill_rect_with_radial_gradient(
                dest_int_rect,
                &RadialGradientData {
                    color_stops: color_stops(),
                    interpolation_method,
                },
                IntPoint {
                    x: inputs.center[0],
                    y: inputs.center[1],
                },
                IntSize {
                    width: inputs.size[0],
                    height: inputs.size[1],
                },
            ),
            FfiImagePaintRecordKind::ConicGradient => recorder.fill_rect_with_conic_gradient(
                dest_int_rect,
                &ConicGradientData {
                    start_angle: inputs.gradient_angle,
                    color_stops: color_stops(),
                    interpolation_method,
                },
                IntPoint {
                    x: inputs.position[0],
                    y: inputs.position[1],
                },
            ),
        }
        let bytes = recorder.into_builder().into_bytes();
        unsafe { consume(context, bytes.as_ptr(), bytes.len()) };
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
pub unsafe extern "C" fn layout_arena_visual_context_tree_node_count(tree: *const c_void) -> usize {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        tree.nodes.len()
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

/// # Safety
///
/// `tree` must be the pointer handed to the callback, used synchronously; `index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_node(
    tree: *const c_void,
    index: usize,
) -> crate::painting::host::FfiVisualContextNodeExport {
    abort_on_panic(|| {
        // SAFETY: `tree` is the VisualContextTree pointer handed out by the host callback wrapper.
        let tree = unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() };
        crate::painting::visual_context::export_node(&tree.nodes[index])
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_spliced_capture_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        paintables
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
        let paintables = arena.paintables().borrow();
        paintables
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
    paintable: PaintableSlotId,
    propagated_text_decorations: bool,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if propagated_text_decorations {
            paintables.invalidate_propagated_text_decoration_caches(arena, paintable);
        } else {
            paintables.invalidate_paint_cache(paintable);
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_computed_svg_path(
    arena: *mut c_void,
    paintable: PaintableSlotId,
) -> *const c_void {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(paintable) {
            return std::ptr::null();
        }
        paintables
            .side(paintable)
            .computed_svg_path
            .as_ref()
            .map_or(std::ptr::null(), |path| path.as_raw())
    })
}

#[repr(C)]
pub struct FfiCaretRectResult {
    pub found: bool,
    pub rect: FfiCssPixelRect,
    pub style_source: *mut c_void,
    pub owner_paintable: *mut c_void,
    pub nearest_self_painting_inline: *mut c_void,
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
            owner_paintable: std::ptr::null_mut(),
            nearest_self_painting_inline: std::ptr::null_mut(),
        };
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        let Some(answer) = crate::painting::caret::caret_rect_for_position(
            arena,
            &paintables,
            node_slots,
            offset,
            affinity_is_downstream,
        ) else {
            return result;
        };
        result.found = true;
        result.rect = answer.rect.into();
        result.style_source = arena.shell_if_live(answer.style_source);
        result.owner_paintable = paintables.data_ref(answer.owner).shell;
        result.nearest_self_painting_inline =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(arena, &paintables, answer.node)
                .map_or(std::ptr::null_mut(), |inline_box| paintables.data_ref(inline_box).shell);
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
        let paintables = arena.paintables().borrow();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        match crate::painting::caret::caret_rect_in_dom_range(arena, &paintables, node_slots, offset) {
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
    block: PaintableSlotId,
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
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(block) {
            return result;
        }
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        let side = paintables.side(block);
        let Some(first_fragment) = side.fragments.first() else {
            return result;
        };
        if !node_slots.contains(&first_fragment.layout_node) {
            return result;
        }
        for target in crate::painting::visual_lines::empty_line_caret_targets(arena, &paintables, block) {
            if target.offset == offset {
                result.has_value = true;
                result.rect = target.rect.into();
                result.style_source =
                    arena.shell_if_live(crate::painting::text_fragment::style_source(arena, first_fragment));
                break;
            }
        }
        result
    })
}

fn with_inline_pieces(
    paintables: &crate::painting::paintable_arena::PaintableArena,
    inline_paintable: PaintableSlotId,
    mut callback: impl FnMut(&InlineBoxPieceRecord, &PaintableData) -> bool,
) {
    let Some(root) = paintables.inline_pieces_root(inline_paintable) else {
        return;
    };
    let data = paintables.data_ref(inline_paintable);
    let root_side = paintables.side(root);
    for piece_index in &paintables.side(inline_paintable).piece_indices {
        let piece = &root_side.inline_box_pieces[*piece_index as usize];
        if !callback(piece, &data) {
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
    inline_paintable: PaintableSlotId,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let Some(root) = paintables.inline_pieces_root(inline_paintable) else {
            return;
        };
        let root_position = crate::painting::paintable_geometry::absolute_position(&paintables, root);
        with_inline_pieces(&paintables, inline_paintable, |piece, _| {
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
    inline_paintable: PaintableSlotId,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let mut has_content = false;
        with_inline_pieces(&paintables, inline_paintable, |piece, _| {
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
    inline_paintable: PaintableSlotId,
) -> FfiOptionalCssPixelPoint {
    abort_on_panic(|| {
        let mut result = FfiOptionalCssPixelPoint {
            has_value: false,
            x: CssPixels::from_raw(0),
            y: CssPixels::from_raw(0),
        };
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let Some(root) = paintables.inline_pieces_root(inline_paintable) else {
            return result;
        };
        let root_position = crate::painting::paintable_geometry::absolute_position(&paintables, root);
        with_inline_pieces(&paintables, inline_paintable, |piece, data| {
            let border_rect = crate::css::css_pixels::CssPixelRect::from(piece.border_box_rect);
            let rect = if piece.is_geometry_only_placeholder {
                border_rect
            } else {
                let padding_rect = piece.shrunken_by_present_edges(border_rect, data.border);
                piece.shrunken_by_present_edges(padding_rect, data.padding)
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
        let paintables = arena.paintables().borrow();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        for line in crate::painting::visual_lines::collect_visual_lines(arena, &paintables, node_slots) {
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
    arena: &LayoutNodeArena,
    node_slots: &[NodeSlotId],
    matches: impl Fn(&FragmentRecord) -> bool,
) -> bool {
    let paintables = arena.paintables().borrow();
    let mut found = false;
    crate::painting::text_fragment::for_each_fragment_of_nodes(arena, &paintables, node_slots, |_, _, fragment| {
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
        has_rendered_text_matching(arena, node_slots, |fragment| fragment.dom_start_offset_in_node < offset)
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
        has_rendered_text_matching(arena, node_slots, |fragment| {
            fragment.dom_start_offset_in_node + fragment.length_in_code_units > offset
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
        let paintables = arena.paintables().borrow();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        let coordinate = crate::painting::visual_lines::caret_inline_coordinate(
            arena,
            &paintables,
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
        let paintables = arena.paintables().borrow();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        crate::painting::visual_lines::offset_closest_to_inline_coordinate(
            arena,
            &paintables,
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
    block: PaintableSlotId,
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
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(block) {
            return facts;
        }
        let Some(fragment) = paintables.side(block).fragments.get(fragment_index as usize) else {
            return facts;
        };
        facts.layout_node = arena.shell_if_live(fragment.layout_node);
        facts.dom_start_offset_in_node = fragment.dom_start_offset_in_node;
        facts.dom_end_offset_in_node = fragment.dom_start_offset_in_node + fragment.length_in_code_units;
        facts.dom_end_offset_with_trailing_whitespace =
            facts.dom_end_offset_in_node + fragment.trailing_whitespace_length_in_code_units;
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
    block: PaintableSlotId,
    fragment_index: u32,
    offset: usize,
    affinity_is_downstream: bool,
) -> FfiCaretMatch {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(block) {
            return FfiCaretMatch::None;
        }
        let Some(fragment) = paintables.side(block).fragments.get(fragment_index as usize) else {
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
    block: PaintableSlotId,
    fragment_index: u32,
    x: CssPixels,
    y: CssPixels,
) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(block) {
            return 0;
        }
        let Some(fragment) = paintables.side(block).fragments.get(fragment_index as usize) else {
            return 0;
        };
        crate::painting::text_fragment::index_in_node_for_point(
            arena,
            &paintables,
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
    block: PaintableSlotId,
    fragment_index: u32,
    offset: usize,
) -> FfiCssPixelRect {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(block) {
            return FfiCssPixelRect::default();
        }
        let Some(fragment) = paintables.side(block).fragments.get(fragment_index as usize) else {
            return FfiCssPixelRect::default();
        };
        let offsets = crate::painting::text_fragment::compute_selection_offsets(
            fragment,
            SELECTION_STATE_START_AND_END,
            offset,
            offset,
        );
        match offsets {
            Some(offsets) => crate::painting::text_fragment::rect_for_selection_offsets(
                arena,
                &paintables,
                fragment,
                offsets,
                || crate::painting::text_fragment::first_available_font(arena, fragment),
            )
            .into(),
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
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        // SAFETY: The caller guarantees the slot span is valid for this synchronous call.
        let node_slots = unsafe { ffi_slice(node_slots, node_slot_count) };
        crate::painting::text_fragment::for_each_fragment_of_nodes(arena, &paintables, node_slots, |_, _, fragment| {
            let fragment_dom_start = fragment.dom_start_offset_in_node;
            let fragment_dom_end = fragment_dom_start + fragment.length_in_code_units;
            if fragment_dom_end <= filter_dom_start || fragment_dom_start >= filter_dom_end {
                return true;
            }
            let rect = crate::painting::text_fragment::range_rect(
                arena,
                &paintables,
                fragment,
                selection_state,
                range_start_offset,
                range_end_offset,
            );
            // SAFETY: The consumer copies the plain-data rect synchronously.
            unsafe { push_rect(context, rect.into()) };
            true
        });
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
    block: PaintableSlotId,
    node: NodeSlotId,
) -> FfiOptionalCssPixelRect {
    abort_on_panic(|| {
        let mut result = FfiOptionalCssPixelRect {
            has_value: false,
            rect: FfiCssPixelRect::default(),
        };
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(block) {
            return result;
        }
        for fragment in &paintables.side(block).fragments {
            if fragment.layout_node != node {
                continue;
            }
            result.has_value = true;
            result.rect = crate::painting::text_fragment::absolute_rect(arena, &paintables, fragment).into();
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
    root: PaintableSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiCssPixelRect),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(root) {
            return;
        }
        paintables.for_each_in_subtree(root, |current| {
            for fragment in &paintables.side(current).fragments {
                let shell = arena.shell_if_live(fragment.layout_node);
                let rect = crate::painting::text_fragment::absolute_rect(arena, &paintables, fragment).into();
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
    paintable: PaintableSlotId,
    indent: usize,
    interactive: bool,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(paintable) {
            return;
        }
        let mut out = Vec::new();
        crate::painting::dump::dump_block_fragments(&mut out, arena, &paintables, paintable, indent, interactive);
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
    inline_paintable: PaintableSlotId,
    indent: usize,
    interactive: bool,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(inline_paintable) {
            return;
        }
        let mut out = Vec::new();
        crate::painting::dump::dump_inline_piece_fragments(
            &mut out,
            arena,
            &paintables,
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
    paintable: PaintableSlotId,
    container_node_id: i64,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if let Some(data) = &paintables.side(paintable).grid_layout_data {
            let json = crate::painting::devtools_layout::serialize_grid_layout(data, container_node_id);
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
    paintable: PaintableSlotId,
    container_node_id: i64,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if let Some(data) = &paintables.side(paintable).flex_layout_data {
            let json = crate::painting::devtools_layout::serialize_flex_layout(data, container_node_id);
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
    paintable: PaintableSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(
        *mut c_void,
        *const crate::layout::FfiUsedGridTrackList,
        *const crate::layout::FfiUsedGridTrackList,
    ),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if let Some(tracks) = &paintables.side(paintable).used_grid_tracks {
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
        let paintables = arena.paintables().borrow();
        paintables
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
        let paintables = arena.paintables().borrow();
        let node = &paintables
            .stacking_context_tree
            .as_ref()
            .expect("no stacking context tree")
            .nodes[index];
        crate::painting::host::FfiStackingContextNodeExport {
            paintable_shell: if paintables.is_live(node.paintable) {
                paintables.data_ref(node.paintable).shell
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
        let paintables = arena.paintables().borrow();
        paintables
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
        let paintables = arena.paintables().borrow();
        paintables
            .visual_context
            .tree
            .as_ref()
            .map_or(std::ptr::null(), |tree| std::ptr::from_ref(tree).cast())
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread; `out`
/// must have room for `capacity` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_state_snapshot(
    arena: *mut c_void,
    out: *mut f32,
    capacity: usize,
) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let snapshot = &paintables.visual_context.scroll_state_snapshot;
        let needed = snapshot.len() * 2;
        if !out.is_null() {
            for (index, offset) in snapshot.iter().enumerate() {
                if index * 2 + 1 >= capacity {
                    break;
                }
                // SAFETY: within the caller's capacity.
                unsafe {
                    *out.add(index * 2) = offset.x;
                    *out.add(index * 2 + 1) = offset.y;
                }
            }
        }
        needed
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_cumulative_scroll_offset_for_node(
    arena: *mut c_void,
    scroll_node_index: usize,
    out_raw: *mut i32,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let state = &paintables.visual_context;
        let offset = state.tree.as_ref().map_or_else(Default::default, |tree| {
            state
                .scroll_state
                .cumulative_offset(tree.scroll_state_slot_for_node(scroll_node_index))
        });
        // SAFETY: the caller passes room for two values.
        unsafe {
            *out_raw = offset.x.raw_value();
            *out_raw.add(1) = offset.y.raw_value();
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        paintables.hit_test_list.as_ref().map_or(0, |list| list.items.len())
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
        let paintables = arena.paintables().borrow();
        let items = &paintables.hit_test_list.as_ref().expect("no hit-test list").items;
        assert_eq!(items.len(), output_length);
        if items.is_empty() {
            return;
        }
        assert!(!output.is_null());
        // SAFETY: The caller provides writable storage for exactly `output_length` exports.
        let output = unsafe { std::slice::from_raw_parts_mut(output, output_length) };
        for (output, item) in output.iter_mut().zip(items.iter()) {
            let rect = |rect: crate::css::css_pixels::CssPixelRect| crate::layout::FfiCssPixelRect::from(rect);
            assert!(
                paintables.is_live(item.paintable),
                "exporting a hit-test item for a non-live paintable"
            );
            let paintable_shell = paintables.data_ref(item.paintable).shell;
            *output = crate::painting::host::FfiHitTestItemExport {
                kind: item.kind as u8,
                paintable_shell,
                chrome_widget_kind: item.chrome_widget_kind,
                has_text_fragment_index: item.text_fragment_index.is_some(),
                text_fragment_index: item.text_fragment_index.unwrap_or(0),
                caret_node_shell: arena.shell_if_live(item.caret_node),
                caret_offset: item.caret_offset,
                rect: rect(item.rect),
                caret_rect: rect(item.caret_rect),
                visual_context_index: item.visual_context_index,
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
        let paintables = arena.paintables().borrow();
        paintables
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
    out_context_index: *mut usize,
    out_display_list_id: *mut u64,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let (context_index, id) = paintables
            .last_recording
            .as_ref()
            .expect("no recording")
            .mask_display_lists[index];
        // SAFETY: the caller passes valid out pointers.
        unsafe {
            *out_context_index = context_index;
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_display_list_bytes(arena: *mut c_void, out_length: *mut usize) -> *const u8 {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let Some(recording) = &paintables.last_recording else {
            // SAFETY: out_length is a live pointer for this synchronous call.
            unsafe { *out_length = 0 };
            return std::ptr::null();
        };
        // SAFETY: as above.
        unsafe { *out_length = recording.display_list_bytes.len() };
        recording.display_list_bytes.as_ptr()
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
                visual_context_index: line.visual_context_index,
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

fn list_generation_of(paintables: &crate::painting::paintable_arena::PaintableArena) -> u64 {
    paintables.hit_test_list.as_ref().map_or(0, |list| list.generation)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_list_generation(arena: *mut c_void) -> u64 {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        list_generation_of(&arena.paintables().borrow())
    })
}

fn with_hit_test_list<R>(
    arena: *mut c_void,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::painting::paintable_arena::PaintableArena) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    {
        let mut paintables = arena.paintables().borrow_mut();
        let Some(list) = paintables.hit_test_list.as_mut() else {
            return default;
        };
        list.build_derived_structures_if_needed();
    }
    let paintables = arena.paintables().borrow();
    let Some(list) = paintables.hit_test_list.as_ref() else {
        return default;
    };
    query(list, &paintables)
}

fn css_point(x_raw: i32, y_raw: i32) -> crate::css::css_pixels::CssPixelPoint {
    crate::css::css_pixels::CssPixelPoint::new(CssPixels::from_raw(x_raw), CssPixels::from_raw(y_raw))
}

fn ffi_topmost(item: Option<crate::painting::hit_test::query::TopmostItem>) -> crate::painting::host::FfiTopmostItem {
    match item {
        Some(item) => crate::painting::host::FfiTopmostItem {
            has_item: true,
            index: item.index,
            local_x: item.local_point.x.raw_value(),
            local_y: item.local_point.y.raw_value(),
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
    x_raw: i32,
    y_raw: i32,
) -> crate::painting::host::FfiTopmostItem {
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, paintables| {
            ffi_topmost(list.find_topmost_item(paintables, &callbacks, css_point(x_raw, y_raw)))
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
    x_raw: i32,
    y_raw: i32,
) -> crate::painting::host::FfiTopmostItemsForCaret {
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, paintables| {
            let (caret_item, hit_item) =
                list.find_topmost_items_for_caret(paintables, &callbacks, css_point(x_raw, y_raw));
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
    x_raw: i32,
    y_raw: i32,
    push_context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, usize),
) {
    abort_on_panic(|| {
        let indices = with_hit_test_list(arena, Vec::new(), |list, paintables| {
            list.hit_test_all(paintables, &callbacks, css_point(x_raw, y_raw))
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
    x_raw: i32,
    y_raw: i32,
    mode: u8,
) -> crate::painting::host::FfiCaretItemForLine {
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, _| {
            match list.caret_item_for_line(
                line_index,
                css_point(x_raw, y_raw),
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
    x_raw: i32,
    y_raw: i32,
    mode: u8,
    scoped: bool,
    respect_clip: bool,
) -> crate::painting::host::FfiClosestLine {
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, _| {
            let closest = list.find_closest_line(
                &callbacks,
                css_point(x_raw, y_raw),
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
                has_block_container_margin_rect: closest.block_container_margin_rect.is_some(),
                block_container_margin_rect: closest.block_container_margin_rect.unwrap_or_default().into(),
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
    pub interpolation_method: [u8; 4],
    pub center: FfiCssPixelPoint,
    pub size: crate::layout::FfiCssPixelSize,
}

/// # Safety
///
/// `gradient_value` must point at a live gradient `StyleValueData` and
/// `current_color_value` at a live `StyleValueData` or null, both borrowed
/// for this call. `out` must point at writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_resolve_gradient_paint_for_size(
    gradient_value: *const c_void,
    current_color: u32,
    current_color_value: *const c_void,
    color_scheme: u8,
    size_width: CssPixels,
    size_height: CssPixels,
    out: *mut FfiResolvedGradientPaint,
    stop_context: *mut c_void,
    append_stop: unsafe extern "C" fn(*mut c_void, u32, f32),
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
                r: (current_color >> 16) as u8,
                g: (current_color >> 8) as u8,
                b: current_color as u8,
                a: (current_color >> 24) as u8,
            }),
            current_color_value,
            length: None,
            channels: None,
        };
        let tile_size = crate::css::css_pixels::CssPixelSize::new(size_width, size_height);
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
                out.size = crate::layout::FfiCssPixelSize {
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
        out.interpolation_method = [
            interpolation_method.interpolation_type as u8,
            interpolation_method.rectangular_color_space as u8,
            interpolation_method.polar_color_space as u8,
            interpolation_method.hue_interpolation_method as u8,
        ];
        for (color, position) in data_stops.colors.iter().zip(data_stops.positions.iter()) {
            // SAFETY: The C++ caller appends into its own storage synchronously.
            unsafe { append_stop(stop_context, color.0, *position) };
        }
    });
}
