/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
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
