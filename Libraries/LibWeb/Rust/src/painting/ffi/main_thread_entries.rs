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

/// The reason the host waits for its document's rows as input or chrome reads boxes: scroll limits, wheel targets and
/// scrollbars, which are read with the overflow every row's commit left measured.
pub(crate) struct InputReadsBoxes {
    _private: (),
}

const INPUT_READS_BOXES: InputReadsBoxes = InputReadsBoxes { _private: () };

/// The reason the host waits for its document's rows as it snaps a scroll container to its snap areas.
pub(crate) struct ScrollSnaps {
    _private: (),
}

const SCROLL_SNAPS: ScrollSnaps = ScrollSnaps { _private: () };

/// Answers `read` from the rows of `host`'s document as of every write the host made, with every row's overflow
/// measured, spending `wait`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn read_measured_rows<R>(
    host: *mut crate::render_state::DocumentHost,
    wait: crate::render_state::LockstepProof,
    read: impl FnOnce(&crate::painting::paint_read::PaintSource<'_>) -> R,
) -> R {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let rows = unsafe { &*host }.fresh_measured_rows(wait);
    let absolute_rects = std::cell::RefCell::default();
    read(&crate::painting::paint_read::PaintSource::over_rows(
        &rows.paintable,
        &absolute_rects,
    ))
}

fn input_reads_boxes() -> crate::render_state::LockstepProof {
    crate::render_state::LockstepProof::for_reason(&INPUT_READS_BOXES)
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_physical_resize_axes(
    host: *mut crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiPhysicalResizeAxes {
    // SAFETY: Guaranteed by the caller.
    let axes = unsafe {
        read_measured_rows(host, input_reads_boxes(), |rows| {
            crate::painting::chrome_geometry::physical_resize_axes(rows, slot)
        })
    };
    FfiPhysicalResizeAxes {
        horizontal: axes.horizontal,
        vertical: axes.vertical,
    }
}

/// # Safety
///
/// As for [`layout_row_paintable_physical_resize_axes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_compute_scrollbar_data(
    host: *mut crate::render_state::DocumentHost,
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
    // SAFETY: Guaranteed by the caller.
    let data = unsafe {
        read_measured_rows(host, input_reads_boxes(), |rows| {
            crate::painting::chrome_geometry::ChromeGeometry {
                arena: rows,
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
            )
        })
    };
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
/// As for [`layout_row_paintable_physical_resize_axes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_minimum_scroll_offset(
    host: *mut crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, input_reads_boxes(), |rows| {
            crate::painting::chrome_geometry::minimum_scroll_offset(rows, slot)
        })
    }
    .into()
}

/// # Safety
///
/// As for [`layout_row_paintable_physical_resize_axes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_maximum_scroll_offset(
    host: *mut crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, input_reads_boxes(), |rows| {
            crate::painting::chrome_geometry::maximum_scroll_offset(rows, slot)
        })
    }
    .into()
}

/// # Safety
///
/// As for [`layout_row_paintable_physical_resize_axes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_wheel_scrollable_axes(
    host: *mut crate::render_state::DocumentHost,
    slot: NodeSlotId,
    viewport_overflow_x: u8,
    viewport_overflow_y: u8,
) -> FfiPhysicalResizeAxes {
    // SAFETY: Guaranteed by the caller.
    let axes = unsafe {
        read_measured_rows(host, input_reads_boxes(), |rows| {
            crate::painting::chrome_geometry::wheel_scrollable_axes(
                rows,
                slot,
                viewport_overflow_x,
                viewport_overflow_y,
            )
        })
    };
    FfiPhysicalResizeAxes {
        horizontal: axes.horizontal,
        vertical: axes.vertical,
    }
}

/// # Safety
///
/// As for [`layout_row_paintable_physical_resize_axes`], and `out_geometry` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_snap_container_geometry(
    host: *mut crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
    out_geometry: *mut crate::painting::host::FfiSnapContainerGeometry,
) -> bool {
    let wait = crate::render_state::LockstepProof::for_reason(&SCROLL_SNAPS);
    // SAFETY: Guaranteed by the caller.
    let geometry = unsafe {
        read_measured_rows(host, wait, |rows| {
            crate::painting::scroll_snap::snap_container_geometry(rows, snap_container)
        })
    };
    let Some(geometry) = geometry else {
        return false;
    };
    // SAFETY: The caller provides writable storage for the geometry.
    unsafe { *out_geometry = geometry };
    true
}

/// # Safety
///
/// As for [`layout_row_paintable_physical_resize_axes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_scroll_snapport_rect(
    host: *mut crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
    scrollport: FfiCssPixelRect,
) -> FfiCssPixelRect {
    let wait = crate::render_state::LockstepProof::for_reason(&SCROLL_SNAPS);
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, wait, |rows| {
            crate::painting::scroll_snap::scroll_snapport_rect(rows, snap_container, scrollport.into())
        })
    }
    .into()
}

/// # Safety
///
/// As for [`layout_row_paintable_physical_resize_axes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_scroll_snap_axes(
    host: *mut crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
) -> crate::painting::host::FfiSnapAxes {
    let wait = crate::render_state::LockstepProof::for_reason(&SCROLL_SNAPS);
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, wait, |rows| {
            crate::painting::scroll_snap::snap_axes_of_scroll_container(rows, snap_container)
        })
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread. The
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread. The
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`; the callbacks in `publish` are
/// called synchronously with their context while the recording's resources are live. `out` must
/// point to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_publish_recording(
    arena: *mut c_void,
    publish: crate::painting::host::FfiRecordingPublishCallbacks,
    out: *mut crate::painting::ffi::FfiPresentedRecording,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    let mut recording = document_host(&main_thread).recording();
    let pending = recording.take_pending_recording();
    if let Some(pending) = pending {
        crate::painting::record::publish::publish_recording(arena, &mut recording, pending, &main_thread, &publish);
    }
    drop(recording);
    // SAFETY: The caller provides writable storage for what it reads of the recording.
    unsafe { out.write(crate::painting::ffi::FfiPresentedRecording::of_last_recording(arena)) };
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`; `describe_node` and `append_text`
/// are called synchronously with `context`, and the slots handed to `describe_node` name the
/// last recording's live paintables.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_recording_trace(
    arena: *mut c_void,
    context: *mut c_void,
    describe_node: unsafe extern "C" fn(*mut c_void, NodeSlotId, *mut c_void),
    append_text: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) -> bool {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { arena_from_handle(arena) };
    let Some(pending) = document_host(&main_thread).recording().take_pending_recording_trace() else {
        return false;
    };
    let Some(recording) = arena.paint_state().borrow().last_recording.clone() else {
        return false;
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
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
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
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

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
/// Input arrays and byte buffers must remain valid and immutable throughout this call;
/// fonts for enabled overlays must be live `Gfx::Font`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_record_display_list(
    arena: *mut c_void,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiRecordingInputs,
) -> bool {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let mut recording = document_host(&main_thread).recording();
    let arena = unsafe { arena_from_handle_mut(arena) };
    // Recording reads overflow, and reading overflow never measures it.
    arena.measure_scrollable_overflow();
    debug_assert!(
        !recording.has_pending_recording(),
        "a recording must be published before the next one starts"
    );
    recording.discard_pending_recording();
    if !arena.paintable_row_is_populated(viewport) || arena.stacking_context_entries(viewport).is_none() {
        return false;
    }
    // The root background paints the union of the viewport and the root's overflow, so it is the
    // one output a viewport move can change. Drop its caches before the frame is published instead
    // of treating the viewport position as a frame-wide input.
    let published_root_background_canvas_rect = recording
        .recorder()
        .published_recording
        .as_ref()
        .map(|recording| recording.root_background_canvas_rect);
    if let Some(published_canvas_rect) = published_root_background_canvas_rect {
        let root = arena
            .paint_state()
            .borrow()
            .root_background_source
            .expect("a recording follows paint preparation")
            .root_layout_node;
        let canvas_rect = crate::painting::record::paint::background_resolution::root_background_canvas_rect(
            &arena.paintable_rows(),
            root,
            inputs.css_viewport_rect.into(),
        );
        if canvas_rect != published_canvas_rect {
            arena.push_paint_damage(root, crate::painting::record::damage::PaintDamage::DRAW_BACKGROUND);
        }
    }
    if inputs.publishes_recording {
        arena.note_publishing_paint_recording_started();
    }
    // The recording reads the document as it is now, and nothing writes the document before it is
    // done.
    let frame = arena.freeze_frame();
    let arena: &LayoutNodeArena = arena;
    let (tree_inputs, root_background_source, trace_recordings) = {
        let paint_state = arena.paint_state().borrow();
        (
            paint_state
                .visual_context
                .last_tree_inputs
                .expect("a recording follows a visual context update"),
            paint_state
                .root_background_source
                .expect("a recording follows paint preparation"),
            paint_state.trace_recordings,
        )
    };
    // SAFETY: The host lends the input arrays and buffers for this call. Only owned output and
    // retained resources escape into the pending recording below.
    let inputs = unsafe { inputs.borrow_recording_inputs(tree_inputs, root_background_source) };
    let job = crate::painting::recording_slot::RecordingJob::new(
        frame,
        recording.take_recorder(),
        viewport,
        trace_recordings,
    );
    let answer = job.run(&inputs);
    recording.accept_recording_answer(answer);
    true
}

/// The host of the document an entry's token was minted for.
fn document_host<'host>(main_thread: &crate::stage::MainThread<'host>) -> &'host crate::render_state::DocumentHost {
    main_thread.host().expect("an entry's token names its document's host")
}
