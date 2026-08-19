/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::{FfiCssPixelPoint, FfiCssPixelRect};
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
pub unsafe extern "C" fn layout_arena_allocate_paintable(
    arena: *mut c_void,
    layout_node: NodeSlotId,
    shell: *mut c_void,
) -> PaintableAllocation {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintables().borrow_mut().allocate(layout_node, shell)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_free_paintable(arena: *mut c_void, slot: PaintableSlotId, generation: u32) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintables().borrow_mut().free(slot, generation);
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
        let mut paintables = arena.paintables().borrow_mut();
        if paintables.is_live(slot) {
            paintables.remove_from_tree(slot);
        }
        if paintables.paintable_of_node(layout_node) == slot {
            paintables.set_paintable_of_node(layout_node, PaintableSlotId::INVALID);
        }
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
pub unsafe extern "C" fn layout_arena_paintable_detach_layout_node(arena: *mut c_void, slot: PaintableSlotId) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(slot) {
            return;
        }
        paintables.update_data(slot, |data| {
            data.layout_node = NodeSlotId::INVALID;
            data.containing_block = PaintableSlotId::INVALID;
        });
        paintables.clear_absolute_rect_memo();
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
pub unsafe extern "C" fn layout_arena_paintable_grid_layout_data(
    arena: *mut c_void,
    paintable: PaintableSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const crate::layout::FfiGridLayoutData),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if let Some(data) = &paintables.side(paintable).grid_layout_data {
            data.with_ffi_view(|view| unsafe { consume(context, view) });
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_flex_layout_data(
    arena: *mut c_void,
    paintable: PaintableSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const crate::layout::FfiFlexLayoutData),
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if let Some(data) = &paintables.side(paintable).flex_layout_data {
            data.with_ffi_view(|view| unsafe { consume(context, view) });
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
pub unsafe extern "C" fn layout_arena_paintable_set_scroll_offset(
    arena: *mut c_void,
    slot: PaintableSlotId,
    offset: FfiCssPixelPoint,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| data.scroll_offset = offset);
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
pub unsafe extern "C" fn layout_arena_paintable_set_overflow_data(
    arena: *mut c_void,
    slot: PaintableSlotId,
    rect: FfiCssPixelRect,
    has_scrollable_overflow: bool,
    present: bool,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| {
                data.overflow = FfiOverflowData {
                    rect,
                    has_scrollable_overflow,
                };
                data.has_overflow = present;
            });
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_set_cached_overflow_data(
    arena: *mut c_void,
    slot: PaintableSlotId,
    rect: FfiCssPixelRect,
    has_scrollable_overflow: bool,
    present: bool,
) {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if paintables.is_live(slot) {
            paintables.update_data(slot, |data| {
                data.cached_overflow = FfiOverflowData {
                    rect,
                    has_scrollable_overflow,
                };
                data.has_cached_overflow = present;
            });
        }
    });
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
/// `arena` must be a live handle from `layout_arena_create`, used on the
/// document thread; `out_matrix` and `out_origin` must hold 16 and 2 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_compute_css_transform(
    arena: *mut c_void,
    paintable: PaintableSlotId,
    callbacks: FfiVisualContextHostCallbacks,
    pixel_ratio: f64,
    out_matrix: *mut f32,
    out_origin: *mut f32,
) -> bool {
    abort_on_panic(|| {
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        if !paintables.is_live(paintable) {
            return false;
        }
        let Some((transform, _is_invertible)) = crate::painting::visual_context::node_values::compute_transform(
            arena,
            &paintables,
            &callbacks,
            paintable,
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_mask_node_shells(
    arena: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, *mut c_void),
    context: *mut c_void,
) {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live arena handle.
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        for slot in paintables.visual_context.paintables_with_mask_nodes.clone() {
            if paintables.is_live(slot) {
                let shell = paintables.data_ref(slot).shell;
                if !shell.is_null() {
                    // SAFETY: Live slots carry their C++ shell pointer.
                    unsafe { callback(context, shell) };
                }
            }
        }
    });
}

#[repr(C)]
pub struct FfiScrollStateSlotExport {
    pub paintable_shell: *mut c_void,
    pub node_index: usize,
    pub parent_slot: usize,
    pub is_sticky: bool,
    pub own_offset: FfiCssPixelPoint,
    pub has_sticky_constraints: bool,
    pub position_relative_to_scroll_ancestor: FfiCssPixelPoint,
    pub border_box_size: crate::layout::FfiCssPixelSize,
    pub scrollport_size: crate::layout::FfiCssPixelSize,
    pub containing_block_region: FfiCssPixelRect,
    pub needs_parent_offset_adjustment: bool,
    pub sticky_insets: FfiStickyInsets,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_state_slot_count(arena: *mut c_void) -> usize {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live arena handle.
        let arena = unsafe { arena_from_handle(arena) };
        arena.paintables().borrow().visual_context.scroll_state.slot_count()
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_state_has_non_viewport_wheel_scroll_target_candidate(
    arena: *mut c_void,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live arena handle.
        let arena = unsafe { arena_from_handle(arena) };
        arena
            .paintables()
            .borrow()
            .visual_context
            .scroll_state
            .has_non_viewport_wheel_scroll_target_candidate
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_state_slot_export(
    arena: *mut c_void,
    slot: usize,
) -> FfiScrollStateSlotExport {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live arena handle.
        let arena = unsafe { arena_from_handle(arena) };
        let paintables = arena.paintables().borrow();
        let state = &paintables.visual_context.scroll_state.states[slot];
        let shell = if paintables.is_live(state.paintable) {
            paintables.data_ref(state.paintable).shell
        } else {
            std::ptr::null_mut()
        };
        let constraints = state.sticky_constraints.unwrap_or_default();
        FfiScrollStateSlotExport {
            paintable_shell: shell,
            node_index: state.node_index,
            parent_slot: state.parent_slot,
            is_sticky: state.is_sticky,
            own_offset: state.own_offset.into(),
            has_sticky_constraints: state.sticky_constraints.is_some(),
            position_relative_to_scroll_ancestor: constraints.position_relative_to_scroll_ancestor.into(),
            border_box_size: constraints.border_box_size.into(),
            scrollport_size: constraints.scrollport_size.into(),
            containing_block_region: constraints.containing_block_region.into(),
            needs_parent_offset_adjustment: constraints.needs_parent_offset_adjustment,
            sticky_insets: constraints.insets,
        }
    })
}
