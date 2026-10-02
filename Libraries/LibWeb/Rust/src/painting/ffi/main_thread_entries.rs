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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
        &scroll_offset_reader(&main_thread, arena, scroll_offset_of_layout_node),
    );
    arena.shell_if_live(&main_thread, scrolling_box)
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
        &scroll_offset_reader(&main_thread, arena, scroll_offset_of_layout_node),
        |node, accepted_delta_x, accepted_delta_y| {
            // SAFETY: The C++ callback appends the shell and deltas to a caller-owned collection.
            unsafe {
                push_scrollable_box(
                    context,
                    arena.node_shell(&main_thread, node),
                    accepted_delta_x,
                    accepted_delta_y,
                );
            }
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    let scrollable_box = crate::painting::scroll_chain::first_wheel_scrollable_box_in_containing_block_chain(
        &arena.paintable_rows(),
        start,
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
    );
    arena.shell_if_live(&main_thread, scrollable_box)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_cleared_from_node(arena: *mut c_void, layout_node: NodeSlotId) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: As above.
    unsafe { paintable_cleared_from_node(&main_thread, arena, layout_node) };
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_event_dispatch_node_shell(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::hit_test::resolve::event_dispatch_shell_for_paintable(&main_thread, arena, slot)
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
    callbacks: FfiVisualContextHostCallbacks,
    visual_context_update_pending: bool,
) -> FfiRenderingPreparationOutcome {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    prepare_for_rendering(
        &main_thread,
        arena,
        &callbacks,
        crate::layout::viewport_propagation::root_background_source(arena),
        visual_context_update_pending,
    )
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let inputs = callbacks.tree_inputs(&main_thread);
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::scroll_snap::for_each_snap_area(&arena.paintable_rows(), snap_container, |slot, area| {
        // SAFETY: The C++ callback copies the geometry into a caller-owned collection.
        unsafe { push_snap_area(context, &raw const area, arena.node_shell(&main_thread, slot)) };
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_visual_viewport_transform(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) -> bool {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    let Some(tree) = &mut paint_state.visual_context.tree else {
        return false;
    };
    let inputs = callbacks.tree_inputs(&main_thread);
    std::sync::Arc::make_mut(tree).set_visual_viewport_transform(
        crate::painting::visual_context::node_values::visual_viewport_transform_data(&inputs),
    );
    true
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
            &main_thread,
            &mut state.scroll_state,
        );
        let mut snapshot = state
            .scroll_state
            .snapshot(callbacks.tree_inputs(&main_thread).device_pixels_per_css_pixel);
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
/// are called synchronously with `context`, and the shells handed to `describe_node` are the
/// last recording's live paintable shells.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_recording_trace(
    arena: *mut c_void,
    context: *mut c_void,
    describe_node: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    append_text: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) -> bool {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
        unsafe { describe_node(context, arena.shell_if_live(&main_thread, slot), (&raw mut name).cast()) };
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
    result.style_source = arena.shell_if_live(&main_thread, answer.style_source);
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
    result.style_source = arena.shell_if_live(&main_thread, answer.style_source);
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
            result.style_source = arena.shell_if_live(
                &main_thread,
                crate::painting::text_fragment::style_source(&paintable_rows, first_fragment),
            );
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
    consume: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiCssPixelRect),
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(root) {
        return;
    }
    crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, root, |current| {
        for fragment in arena.paintable_side_data(current).fragments() {
            let shell = arena.shell_if_live(&main_thread, fragment.layout_node);
            let rect = crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into();
            // SAFETY: The consumer copies its plain-data arguments synchronously.
            unsafe { consume(context, shell, rect) };
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
            caret_node_shell: arena.shell_if_live(&main_thread, item.caret_node),
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    with_hit_test_list_items_only(arena, std::ptr::null_mut(), |list, arena| {
        list.item_target_shell(&main_thread, arena, item_index)
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    with_hit_test_list_items_only(arena, std::ptr::null_mut(), |list, arena| {
        let (shell, allow_pseudo_fallback) = list.item_dispatch_shell(&main_thread, arena, item_index);
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    with_hit_test_list_items_only(arena, Default::default(), |list, arena| {
        list.resolve_hit(&main_thread, arena, item_index, local_point.into())
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    with_hit_test_list_items_only(arena, Default::default(), |list, arena| {
        list.resolve_caret(
            &main_thread,
            arena,
            item_index,
            local_point.into(),
            crate::painting::hit_test::caret::CaretPositionType::from_u8(position_type),
        )
    })
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
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
            unsafe {
                resolve_paint_server(
                    arena.shell_if_live(&main_thread, slot),
                    is_stroke,
                    (&raw mut published).cast(),
                );
            }
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
        let shell = arena.shell_if_live(&main_thread, slot);
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
