/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scrolling_box_for_scroll_step(
    arena: *mut c_void,
    target: NodeSlotId,
    viewport: NodeSlotId,
    delta: FfiCssPixelPoint,
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
) -> NodeSlotId {
    let arena = unsafe { arena_from_handle(arena) };
    arena.measure_scrollable_overflow();
    crate::painting::scroll_chain::scrolling_box_for_scroll_step(
        &arena.paintable_rows(),
        target,
        viewport,
        delta.into(),
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// host callback receives the slots of live rows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_for_each_wheel_scrollable_box_in_containing_block_chain(
    arena: *mut c_void,
    start: NodeSlotId,
    wheel_delta_x: f64,
    wheel_delta_y: f64,
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
    context: *mut c_void,
    push_scrollable_box: unsafe extern "C" fn(*mut c_void, NodeSlotId, f64, f64),
) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.measure_scrollable_overflow();
    crate::painting::scroll_chain::for_each_wheel_scrollable_box_in_containing_block_chain(
        &arena.paintable_rows(),
        start,
        wheel_delta_x,
        wheel_delta_y,
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
        |node, accepted_delta_x, accepted_delta_y| {
            // SAFETY: The C++ callback appends the slot and deltas to a caller-owned collection.
            unsafe { push_scrollable_box(context, node, accepted_delta_x, accepted_delta_y) };
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
) -> NodeSlotId {
    let arena = unsafe { arena_from_handle(arena) };
    arena.measure_scrollable_overflow();
    crate::painting::scroll_chain::first_wheel_scrollable_box_in_containing_block_chain(
        &arena.paintable_rows(),
        start,
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_cleared_from_node(arena: *mut c_void, layout_node: NodeSlotId) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: As above.
    unsafe {
        paintable_cleared_from_node(
            crate::layout::tree_mutation::HostCalls::Now(&main_thread),
            arena,
            layout_node,
        );
    };
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_layout_node_shell(arena: *mut c_void, slot: NodeSlotId) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return std::ptr::null_mut();
    }
    arena.shell_if_live(&main_thread, slot)
}

/// # Safety
///
/// `arena` must be a live arena used on the document thread. Host callbacks must
/// remain valid for this call and must not mutate layout geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_prepare_for_rendering(
    arena: *mut c_void,
    visual_context_update_pending: bool,
) -> FfiRenderingPreparationOutcome {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    prepare_for_rendering(
        &main_thread,
        arena,
        crate::layout::viewport_propagation::root_background_source(arena),
        visual_context_update_pending,
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// host callback receives each snap area's geometry, valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_for_each_snap_area(
    arena: *mut c_void,
    snap_container: NodeSlotId,
    context: *mut c_void,
    push_snap_area: unsafe extern "C" fn(*mut c_void, *const crate::painting::host::FfiSnapAreaGeometry),
) {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::scroll_snap::for_each_snap_area(&arena.paintable_rows(), snap_container, |slot, mut area| {
        // A re-snap right after layout runs ahead of the visual context tree that names the area's
        // element, so the area is named by what the build stamped onto its row.
        area.node_id = arena.unique_node_ids().id(slot);
        // SAFETY: The C++ callback copies the geometry into a caller-owned collection.
        unsafe { push_snap_area(context, &raw const area) };
    });
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    let Some(pending) = arena.paint_state().borrow_mut().pending_recording.take() else {
        return 0;
    };
    crate::painting::record::publish::publish_recording(arena, pending, &main_thread, &publish)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `describe_node` and `append_text`
/// are called synchronously with `context`, and the slots handed to `describe_node` name the
/// last recording's live paintables.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_recording_trace(
    arena: *mut c_void,
    context: *mut c_void,
    describe_node: unsafe extern "C" fn(*mut c_void, NodeSlotId, *mut c_void),
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
        // SAFETY: the host copies the description synchronously into the sink.
        unsafe { describe_node(context, slot, (&raw mut name).cast()) };
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
        style_source: NodeSlotId::INVALID,
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
    result.style_source = answer.style_source;
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
        style_source: NodeSlotId::INVALID,
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
    result.style_source = answer.style_source;
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
pub unsafe extern "C" fn layout_arena_paintable_empty_line_caret_rect(
    arena: *mut c_void,
    block: NodeSlotId,
    primary: NodeSlotId,
    offset: usize,
) -> FfiEmptyLineCaretRect {
    let mut result = FfiEmptyLineCaretRect {
        has_value: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
    };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(block) {
        return result;
    }
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    let side = arena.committed_side_data(block);
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
            result.style_source = crate::painting::text_fragment::style_source(&paintable_rows, first_fragment);
            break;
        }
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
    consume: unsafe extern "C" fn(*mut c_void, NodeSlotId, FfiCssPixelRect),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(root) {
        return;
    }
    crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, root, |current| {
        for fragment in arena.committed_side_data(current).fragments() {
            let rect = crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into();
            // SAFETY: The consumer copies its plain-data arguments synchronously.
            unsafe { consume(context, fragment.layout_node, rect) };
        }
    });
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    with_hit_test_list_spatial_indexes_and_visual_context_tree(arena, true, Default::default(), |list, tree, arena| {
        let closest = list.find_closest_line(
            &main_thread,
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    with_hit_test_list_and_caret_lines(arena, Default::default(), |list, arena| {
        match list.adjacent_line(
            &main_thread,
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
