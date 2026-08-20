/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::css::css_pixels::CssPixels;
use crate::layout::FfiCssPixelPoint;
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

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_set_selection_state(
    arena: *mut c_void,
    slot: PaintableSlotId,
    state: u8,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| data.selection_state = state);
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_set_fragment_selection_state(
    arena: *mut c_void,
    slot: PaintableSlotId,
    fragment_index: u32,
    state: u8,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        if !paintables.is_live(slot) {
            return;
        }
        if let Some(fragment) = paintables.side_mut(slot).fragments.get_mut(fragment_index as usize) {
            fragment.selection_state = state;
        }
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
pub unsafe extern "C" fn layout_arena_paintable_translate_scrollable_overflow(
    arena: *mut c_void,
    slot: PaintableSlotId,
    delta: FfiCssPixelPoint,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| {
                if data.has_overflow {
                    data.overflow.rect.x += delta.x;
                    data.overflow.rect.y += delta.y;
                }
            });
        }
    });
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
        let (mut tree, scroll_state, paintables_with_mask_nodes) = {
            let paintables = arena.paintables().borrow();
            if !paintables.is_live(viewport) {
                return false;
            }
            crate::painting::visual_context::build::build_visual_context_tree(arena, &paintables, &callbacks, viewport)
        };
        let mut paintables = arena.paintables().borrow_mut();
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
pub unsafe extern "C" fn layout_arena_clear_visual_context_tree(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        paintables.visual_context.tree = None;
        paintables.visual_context.paintables_with_mask_nodes.clear();
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
pub unsafe extern "C" fn layout_arena_paint_push_text_span(
    sink: *mut c_void,
    span: crate::painting::host::FfiTextSpan,
) {
    abort_on_panic(|| {
        // SAFETY: `sink` is the TextSpanSink pointer handed out by FfiPaintHostCallbacks::text_spans.
        let sink = unsafe { &mut *sink.cast::<crate::painting::host::TextSpanSink>() };
        sink.spans.push(span);
    });
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_text_shadow(
    sink: *mut c_void,
    layer: crate::painting::host::FfiTextShadowLayer,
) {
    abort_on_panic(|| {
        // SAFETY: as above.
        let sink = unsafe { &mut *sink.cast::<crate::painting::host::TextSpanSink>() };
        sink.shadows.push(layer);
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
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_glyph_intercept(sink: *mut c_void, value: f32) {
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by FfiPaintHostCallbacks::glyph_intercepts.
        let intercepts = unsafe { &mut *sink.cast::<Vec<f32>>() };
        intercepts.push(value);
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
pub unsafe extern "C" fn layout_arena_clear_paint_cache_sources(arena: *mut c_void) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let mut paintables = arena.paintables().borrow_mut();
        paintables.paint_command_cache_source = None;
        paintables.hit_test_item_cache_source = None;
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
/// `arena` must be a live handle from `layout_arena_create`; `index` in range. The returned
/// path pointer borrows the item's path and is only valid until the next recording.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item(
    arena: *mut c_void,
    index: usize,
) -> crate::painting::host::FfiHitTestItemExport {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let item = &paintables.hit_test_list.as_ref().expect("no hit-test list").items[index];
        let rect = |rect: crate::css::css_pixels::CssPixelRect| crate::layout::FfiCssPixelRect::from(rect);
        let paintable_shell = if paintables.is_live(item.paintable) {
            paintables.data_ref(item.paintable).shell
        } else {
            std::ptr::null_mut()
        };
        crate::painting::host::FfiHitTestItemExport {
            kind: item.kind as u8,
            paintable_shell,
            chrome_widget_kind: item.chrome_widget_kind,
            has_text_fragment_index: item.text_fragment_index.is_some(),
            text_fragment_index: item.text_fragment_index.unwrap_or(0),
            caret_node_shell: arena.shell_if_live(item.caret_node),
            caret_offset: item.caret_offset,
            rect: rect(item.rect),
            caret_rect: rect(item.caret_rect),
            has_caret_line_index: item.caret_line_index.is_some(),
            caret_line_index: item.caret_line_index.unwrap_or(0),
            has_caret_line_rect: item.caret_line_rect.is_some(),
            caret_line_rect: rect(item.caret_line_rect.unwrap_or_default()),
            has_block_container_margin_rect: item.block_container_margin_rect.is_some(),
            block_container_margin_rect: rect(item.block_container_margin_rect.unwrap_or_default()),
            visual_context_index: item.visual_context_index,
            border_radii: item.border_radii.values.map(|value| value.raw_value()),
            path: item
                .path
                .as_ref()
                .map_or(std::ptr::null(), |path| path.as_raw().cast_const()),
            winding_rule: item.winding_rule,
        }
    })
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
    abort_on_panic(|| {
        with_hit_test_list(arena, 0, |list, _| {
            list.build_derived_structures_if_needed();
            list.caret_lines.len()
        })
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
    abort_on_panic(|| {
        with_hit_test_list(arena, Default::default(), |list, _| {
            list.build_derived_structures_if_needed();
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
    abort_on_panic(|| {
        with_hit_test_list(arena, 0, |list, _| {
            list.build_derived_structures_if_needed();
            list.caret_item_indices.len()
        })
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `caret_item_index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_item_index(arena: *mut c_void, caret_item_index: usize) -> usize {
    abort_on_panic(|| {
        with_hit_test_list(arena, usize::MAX, |list, _| {
            list.build_derived_structures_if_needed();
            list.caret_item_indices[caret_item_index]
        })
    })
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
    query: impl FnOnce(&mut crate::painting::hit_test::HitTestList, &crate::painting::paintable_arena::PaintableArena) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let mut paintables = arena.paintables().borrow_mut();
    let crate::painting::paintable_arena::PaintableArena { hit_test_list, .. } = &mut *paintables;
    let Some(list) = hit_test_list.as_mut() else {
        return default;
    };
    let list_ptr: *mut crate::painting::hit_test::HitTestList = list;
    // SAFETY: `list` lives inside `paintables` and is disjoint from the arena data the queries
    // read; nothing else touches it during the call.
    query(unsafe { &mut *list_ptr }, &paintables)
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
pub unsafe extern "C" fn layout_arena_hit_test_push_empty_line_caret_target(
    sink: *mut c_void,
    target: crate::painting::host::FfiEmptyLineCaretTarget,
) {
    abort_on_panic(|| {
        // SAFETY: `sink` is the Vec pointer handed out by FfiHitTestHostCallbacks::empty_line_caret_targets.
        let targets = unsafe { &mut *sink.cast::<Vec<crate::painting::host::FfiEmptyLineCaretTarget>>() };
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
