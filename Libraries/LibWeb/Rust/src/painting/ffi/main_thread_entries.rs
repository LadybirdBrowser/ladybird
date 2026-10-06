/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;
use crate::painting::host::FfiCaretAt;
use crate::painting::host::FfiVisualContextTreeInputs;
use crate::painting::paint_passes::{PassEffect, pending_preparation, run as run_paint_pass};

crate::stage::main_thread_ffi_entries!();

/// Answers `read` from the rows of `host`'s document as of every write the host made, spending `wait`, with every
/// row's overflow measured, as input and chrome read boxes: scroll limits, wheel targets, scrollbars and snap areas.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn read_measured_rows<R>(
    host: &crate::render_state::DocumentHost,
    wait: impl crate::render_state::RenderWait,
    read: impl FnOnce(&crate::painting::paint_read::PaintSource<'_>) -> R,
) -> R {
    host.read_rows(wait, true, read)
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_physical_resize_axes(
    host: &crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiPhysicalResizeAxes {
    // SAFETY: Guaranteed by the caller.
    let axes = unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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
    host: &crate::render_state::DocumentHost,
    slot: NodeSlotId,
    direction: ScrollDirection,
    metrics: FfiChromeMetrics,
    enlarged: bool,
    has_device_scroll_offset: bool,
    device_scroll_offset: f32,
    device_pixels_per_css_pixel: f64,
) -> FfiOptionalScrollbarData {
    // SAFETY: Guaranteed by the caller.
    let data = unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
            crate::painting::chrome_geometry::ChromeGeometry { arena: rows, metrics }.compute_scrollbar_data(
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
    host: &crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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
    host: &crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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
    host: &crate::render_state::DocumentHost,
    slot: NodeSlotId,
) -> FfiPhysicalResizeAxes {
    // SAFETY: Guaranteed by the caller.
    let axes = unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
            crate::painting::chrome_geometry::wheel_scrollable_axes(rows, slot)
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
    host: &crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
    out_geometry: *mut crate::painting::host::FfiSnapContainerGeometry,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let geometry = unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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
    host: &crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
    scrollport: FfiCssPixelRect,
) -> FfiCssPixelRect {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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
    host: &crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
) -> crate::painting::host::FfiSnapAxes {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
            crate::painting::scroll_snap::snap_axes_of_scroll_container(rows, snap_container)
        })
    }
}

/// The box a scroll brings the caret at `offset` in the text of `text` into view in, which it scrolls to `*offset_out`, or
/// an invalid slot for none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_scroll_target_for_text_position(
    host: &crate::render_state::DocumentHost,
    text: NodeSlotId,
    offset: usize,
    affinity_is_downstream: bool,
    scroll_block_axis: bool,
    offset_out: &mut FfiCssPixelPoint,
) -> NodeSlotId {
    // SAFETY: Guaranteed by the caller.
    let target = unsafe {
        read_arena(
            host,
            super::node_read(),
            (text, offset, affinity_is_downstream, scroll_block_axis),
            |arena, (text, offset, affinity_is_downstream, scroll_block_axis)| {
                arena.measure_scrollable_overflow();
                crate::painting::scroll_chain::scroll_target_for_text_position(
                    &arena.paintable_rows(),
                    text,
                    offset,
                    affinity_is_downstream,
                    scroll_block_axis,
                )
            },
        )
    };
    let Some((container, scroll_offset)) = target else {
        return NodeSlotId::INVALID;
    };
    *offset_out = scroll_offset.into();
    container
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_scrolling_box_for_scroll_step(
    host: &crate::render_state::DocumentHost,
    target: NodeSlotId,
    viewport: NodeSlotId,
    delta: FfiCssPixelPoint,
) -> NodeSlotId {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(
            host,
            super::node_read(),
            (target, viewport, delta),
            |arena, (target, viewport, delta)| {
                arena.measure_scrollable_overflow();
                crate::painting::scroll_chain::scrolling_box_for_scroll_step(
                    &arena.paintable_rows(),
                    target,
                    viewport,
                    delta.into(),
                )
            },
        )
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The host callback receives the slots of live rows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_for_each_wheel_scrollable_box_in_containing_block_chain(
    host: &crate::render_state::DocumentHost,
    start: NodeSlotId,
    wheel_delta_x: f64,
    wheel_delta_y: f64,
    context: *mut c_void,
    push_scrollable_box: unsafe extern "C" fn(*mut c_void, NodeSlotId, f64, f64),
) {
    // SAFETY: Guaranteed by the caller.
    let boxes = unsafe {
        read_arena(
            host,
            super::node_read(),
            (start, wheel_delta_x, wheel_delta_y),
            |arena, (start, wheel_delta_x, wheel_delta_y)| {
                arena.measure_scrollable_overflow();
                let mut boxes = Vec::new();
                crate::painting::scroll_chain::for_each_wheel_scrollable_box_in_containing_block_chain(
                    &arena.paintable_rows(),
                    start,
                    wheel_delta_x,
                    wheel_delta_y,
                    |node, accepted_delta_x, accepted_delta_y| boxes.push((node, accepted_delta_x, accepted_delta_y)),
                );
                boxes
            },
        )
    };
    for (node, accepted_delta_x, accepted_delta_y) in boxes {
        // SAFETY: The C++ callback appends the slot and deltas to a caller-owned collection.
        unsafe { push_scrollable_box(context, node, accepted_delta_x, accepted_delta_y) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_first_wheel_scrollable_box_in_containing_block_chain(
    host: &crate::render_state::DocumentHost,
    start: NodeSlotId,
) -> NodeSlotId {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), start, |arena, start| {
            arena.measure_scrollable_overflow();
            crate::painting::scroll_chain::first_wheel_scrollable_box_in_containing_block_chain(
                &arena.paintable_rows(),
                start,
            )
        })
    }
}

/// Prepares the document of `host` for rendering, and stores the scroll offsets the new overflow moved out of range.
/// The viewport the preparation reads is asked of `inputs_of` only when there is something to prepare.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. Host callbacks must remain valid for this call, and
/// `inputs_of` must answer synchronously from `document`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_prepare_for_rendering(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    visual_context_update_pending: bool,
    document: *mut c_void,
    inputs_of: unsafe extern "C" fn(*mut c_void) -> FfiVisualContextTreeInputs,
) -> crate::painting::paint_passes::FfiRenderingPreparationOutcome {
    let Some(pending) = pending_preparation(read, host) else {
        return Default::default();
    };
    // SAFETY: Guaranteed by the caller.
    let inputs = unsafe { inputs_of(document) };
    let prepared = run_paint_pass(read, host, PassEffect::RewritesRows, move |arena| {
        pending.prepare(arena, (!visual_context_update_pending).then_some(&inputs))
    });
    if !prepared.clamped_scroll_offsets.is_empty() {
        // SAFETY: Guaranteed by the caller.
        let main_thread = unsafe { main_thread(host) };
        if let Some(geometry_host) = host.host_tables().geometry_host.get() {
            for (slot, offset) in prepared.clamped_scroll_offsets {
                // SAFETY: The pass clamped the offsets of live rows, and the host holds no borrow of the arena.
                unsafe { geometry_host.set_scroll_offset(&main_thread, read, slot, offset.into()) };
            }
        }
    }
    prepared.outcome
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_update_accumulated_visual_contexts(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    inputs: FfiVisualContextTreeInputs,
) -> crate::painting::host::FfiVisualContextUpdateOutcome {
    run_paint_pass(read, host, PassEffect::RewritesRows, |arena| {
        crate::painting::paint_passes::update_accumulated_visual_contexts(arena, viewport, inputs)
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_update_visual_viewport_transform(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    inputs: FfiVisualContextTreeInputs,
) {
    run_paint_pass(read, host, PassEffect::StalesPaintPreparation, |arena| {
        crate::painting::paint_passes::update_visual_viewport_transform(arena, &inputs);
    });
}

/// Starts an update pass of the compositor animations of `host`'s document, with none published.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_begin_compositor_animation_update(host: &crate::render_state::DocumentHost) {
    host.begin_compositor_animation_update();
}

/// Publishes the effect's pending animations: they become the ones it retains, and copies join the compositor
/// animations of the current update pass of `host`'s document.
///
/// # Safety
///
/// `state` must be a live effect state handle and `host` a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_publish_pending(
    state: *mut c_void,
    host: &crate::render_state::DocumentHost,
    reuse_retained_timing_anchors: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let animations = unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }
        .publish_pending(reuse_retained_timing_anchors);
    // SAFETY: As above.
    host.publish_compositor_animations(animations);
}

/// Ends the current update pass of the compositor animations of `host`'s document, and gives the visual context tree
/// what its effects published where `publish_pending`, or none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_publish_compositor_animations(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    publish_pending: bool,
) -> crate::painting::host::FfiCompositorAnimationPublishOutcome {
    let mut animations = host.take_compositor_animations();
    if !publish_pending {
        animations.clear();
    }
    run_paint_pass(read, host, PassEffect::KeepsPaintPreparation, |arena| {
        crate::painting::visual_context::publish_compositor_animations(
            &mut arena.paint_state().borrow_mut().visual_context,
            animations,
        )
    })
}

/// Resolves the SVG paint resources the enrolled rows of `host`'s document name: the render state answers what to
/// resolve, the host resolves it from the DOM, and the render state publishes what it resolved. Answers whether a
/// published resource changed.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and both resolvers must answer synchronously for the
/// row they are handed and only push into the sink whose pointer they receive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_sync_svg_paint_resources(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    context: *mut c_void,
    resolve_filter: unsafe extern "C" fn(*mut c_void, NodeSlotId, *const c_void, *mut c_void) -> bool,
    resolve_paint_server: unsafe extern "C" fn(*mut c_void, NodeSlotId, bool, *mut c_void),
) -> bool {
    use crate::painting::paint_passes::{ResolvedSvgPaintResource, SvgPaintResourceRequest};
    use crate::painting::svg_paint_resources::{PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind};
    let requests = run_paint_pass(read, host, PassEffect::KeepsPaintPreparation, |arena| {
        crate::painting::paint_passes::svg_paint_resource_requests(arena)
    });
    let Some(requests) = requests else {
        return false;
    };
    let resolved = requests
        .into_iter()
        .map(|request| match request {
            SvgPaintResourceRequest::PaintServer { slot, kind } => {
                let mut published = PublishedSvgPaintServer::None;
                // SAFETY: The host resolves synchronously for the row, and only pushes into the sink it is handed.
                unsafe {
                    resolve_paint_server(
                        context,
                        slot,
                        kind == SvgPaintResourceKind::Stroke,
                        (&raw mut published).cast(),
                    );
                }
                ResolvedSvgPaintResource::PaintServer { slot, kind, published }
            }
            SvgPaintResourceRequest::Filter { slot, kind, urls } => {
                let mut published = PublishedSvgFilter::default();
                for url in &urls {
                    let mut primitives: Vec<SvgFilterPrimitive> = Vec::new();
                    // SAFETY: The host resolves synchronously for the row, and only pushes into the primitive list it
                    // is handed as its sink.
                    let found =
                        unsafe { resolve_filter(context, slot, url.pointer().cast(), (&raw mut primitives).cast()) };
                    published = PublishedSvgFilter {
                        failed: !found,
                        primitives: if found { primitives } else { Vec::new() },
                    };
                    if published.failed {
                        break;
                    }
                }
                ResolvedSvgPaintResource::Filter { slot, kind, published }
            }
        })
        .collect();
    run_paint_pass(read, host, PassEffect::StalesPaintPreparation, |arena| {
        crate::painting::paint_passes::publish_resolved_svg_paint_resources(arena, resolved)
    })
}

/// Re-reads the scroll containers' offsets when something invalidated them since the last refresh, resolves the
/// sticky nodes' offsets on top of them, and hands the dense device-pixel snapshot to `publish`; otherwise the caller
/// keeps its copy.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread; `publish` is called synchronously with `sink` and a
/// view of the snapshot that is valid only for the duration of that call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_refresh_scroll_state(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    device_pixels_per_css_pixel: f64,
    sink: *mut c_void,
    publish: unsafe extern "C" fn(*mut c_void, *const libgfx_rust::FloatPoint, usize),
) {
    let snapshot = run_paint_pass(read, host, PassEffect::RewritesRows, |arena| {
        crate::painting::paint_passes::refresh_scroll_state(arena, device_pixels_per_css_pixel)
    });
    if let Some(snapshot) = snapshot {
        // SAFETY: The C++ sink copies the offsets synchronously.
        unsafe { publish(sink, snapshot.as_ptr(), snapshot.len()) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The host callback receives each snap area's
/// geometry, valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_for_each_snap_area(
    host: &crate::render_state::DocumentHost,
    snap_container: NodeSlotId,
    context: *mut c_void,
    push_snap_area: unsafe extern "C" fn(*mut c_void, *const crate::painting::host::FfiSnapAreaGeometry),
) {
    // SAFETY: Guaranteed by the caller.
    let areas = unsafe {
        read_arena(host, super::node_read(), snap_container, |arena, snap_container| {
            let mut areas = Vec::new();
            crate::painting::scroll_snap::for_each_snap_area(
                &arena.paintable_rows(),
                snap_container,
                |slot, mut area| {
                    // A re-snap right after layout runs ahead of the visual context tree that names the area's
                    // element, so the area is named by what the build stamped onto its row.
                    area.node_id = arena.unique_node_ids().id(slot);
                    areas.push(area);
                },
            );
            areas
        })
    };
    for area in &areas {
        // SAFETY: The C++ callback copies the geometry into a caller-owned collection.
        unsafe { push_snap_area(context, area) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread; the callbacks in `publish` are called
/// synchronously with their context while the recording's resources are live. `out` must point to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_publish_recording(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    publish: crate::painting::host::FfiRecordingPublishCallbacks,
    out: *mut crate::painting::ffi::FfiPresentedRecording,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    let mut recording = host.recording();
    if let Some(publication) = recording.take_publication() {
        crate::painting::record::publish::publish_recording(host, publication, &main_thread, &publish);
    }
    drop(recording);
    let presented = unsafe {
        read_arena(host, read, (), |arena, ()| {
            crate::painting::ffi::FfiPresentedRecording::of_last_recording(arena)
        })
    };
    // SAFETY: The caller provides writable storage for what it reads of the recording.
    unsafe { out.write(presented) };
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread; `describe_node` and `append_text` are called
/// synchronously with `context`, and the slots handed to `describe_node` name the last recording's live paintables.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_take_recording_trace(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    context: *mut c_void,
    describe_node: unsafe extern "C" fn(*mut c_void, NodeSlotId, *mut c_void),
    append_text: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) -> bool {
    let Some(pending) = host.recording().take_pending_recording_trace() else {
        return false;
    };
    // SAFETY: As above.
    let recording = unsafe {
        read_arena(host, read, (), |arena, ()| {
            arena.paint_state().borrow().last_recording.clone()
        })
    };
    let Some(recording) = recording else {
        return false;
    };
    let Some(log) = recording.capture_log.as_ref() else {
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
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_for_each_subtree_fragment_rect(
    host: &crate::render_state::DocumentHost,
    root: NodeSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, NodeSlotId, FfiCssPixelRect),
) {
    // SAFETY: Guaranteed by the caller.
    let rects = unsafe {
        read_arena(host, super::node_read(), root, |arena, root| {
            let paintable_rows = arena.paintable_rows();
            let mut rects = Vec::new();
            if !paintable_rows.paintable_row_is_populated(root) {
                return rects;
            }
            crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, root, |current| {
                for fragment in arena.committed_side_data(current).fragments() {
                    rects.push((
                        fragment.layout_node,
                        crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into(),
                    ));
                }
            });
            rects
        })
    };
    for (layout_node, rect) in rects {
        // SAFETY: The consumer copies its plain-data arguments synchronously.
        unsafe { consume(context, layout_node, rect) };
    }
}

/// The recorder state of `host`'s document, for a recording to take, once the clock lease that may have it ended and the
/// last recording was published.
fn take_recorder_for_recording(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
) -> crate::painting::record::recorder_state::RecorderState {
    host.end_clock_lease_waiting(read);
    let mut recording = host.recording();
    debug_assert!(
        !recording.has_pending_recording() && !recording.has_recording_in_flight(),
        "a recording must be published before the next one starts"
    );
    recording.discard_pending_recording();
    recording.take_recorder()
}

/// What freezing the frame of `host`'s document's `viewport` for a recording with `inputs` reads.
fn frame_inputs(
    host: &crate::render_state::DocumentHost,
    viewport: NodeSlotId,
    inputs: &crate::painting::host::FfiRecordingInputs,
    recorder: &crate::painting::record::recorder_state::RecorderState,
) -> crate::painting::recording_slot::FrameInputs {
    crate::painting::recording_slot::FrameInputs {
        viewport,
        css_viewport_rect: inputs.css_viewport_rect.into(),
        publishes_recording: inputs.publishes_recording,
        published_root_background_canvas_rect: recorder
            .published_recording
            .as_ref()
            .map(|recording| recording.root_background_canvas_rect),
        hit_test_item_capacity_hint: host.recording().hit_test_item_capacity_hint(),
    }
}

/// Records `host`'s document's viewport with `inputs` on the Paint thread, in step with the host, which publishes the
/// recording and presents nothing. Answers whether the viewport had a box to record.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. Input arrays and byte buffers must be valid and
/// immutable for this call; fonts for enabled overlays must be live `Gfx::Font`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_record_display_list(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiRecordingInputs,
) -> FfiRecordingStart {
    let recorder = take_recorder_for_recording(host, read);
    let frame_inputs = frame_inputs(host, viewport, &inputs, &recorder);
    // SAFETY: The host lends the input arrays and buffers for this call, and the inputs copy what they read of them.
    let inputs = unsafe { inputs.recording_inputs() };
    // SAFETY: Guaranteed by the caller.
    let Some(frame) = (unsafe {
        read_arena(
            host,
            read,
            frame_inputs,
            crate::painting::recording_slot::freeze_recording_frame,
        )
    }) else {
        host.recording().give_back_recorder(recorder);
        return FfiRecordingStart::NothingToRecord;
    };
    let inputs = inputs.for_frame(frame.tree_inputs, frame.root_background_source);
    let answer =
        crate::painting::recording_slot::RecordingJob::new(frame, recorder, viewport, None).run_on_paint_thread(inputs);
    host.recording().accept_recording_answer(answer);
    FfiRecordingStart::Recorded
}

/// Commits the frame of `host`'s document as the rendering update leaves it to the render owner, with `recorder`, which
/// samples it beside the event loop at `timestamp`, the time of the document's timeline the update sampled its
/// animations at, and hands it to the Paint thread, which presents it with `presentation`. The frame flies until the
/// host takes it in.
///
/// # Safety
///
/// `presentation` must name a presentation the host gives up.
unsafe fn commit_frame(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    recorder: crate::painting::record::recorder_state::RecorderState,
    content: crate::render_state::CommittedContent,
    held_for_testing: bool,
    timestamp: f64,
    presentation: crate::painting::ffi::FfiPresentation,
) {
    let commit = crate::render_state::CommittedFrame {
        content,
        recorder,
        // SAFETY: Guaranteed by the caller.
        presentation: unsafe { crate::painting::presentation::Presentation::adopt(presentation) }
            .expect("a committed frame is presented"),
        timestamp,
        held_for_testing,
    };
    let flight = host.commit_rendering_update(read, commit);
    host.recording().fly(flight);
}

/// Commits the frame of `host`'s document as a recording of its viewport with `inputs` (see [`commit_frame`]).
///
/// # Safety
///
/// As for [`render_state_record_display_list`]. `presentation` must name a presentation the host gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_commit_recorded_frame(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiRecordingInputs,
    timestamp: f64,
    presentation: crate::painting::ffi::FfiPresentation,
) {
    let recorder = take_recorder_for_recording(host, read);
    let content = crate::render_state::CommittedContent::Recording {
        frame_inputs: frame_inputs(host, viewport, &inputs, &recorder),
        // SAFETY: The host lends the input arrays and buffers for this call, and the inputs copy what they read of them.
        inputs: unsafe { inputs.recording_inputs() },
    };
    let held_for_testing = crate::painting::recording_slot::take_recording_hold_for_testing();
    // SAFETY: Guaranteed by the caller.
    unsafe { commit_frame(host, read, recorder, content, held_for_testing, timestamp, presentation) };
}

/// Commits the frame of `host`'s document as one that keeps the display list the compositor has, and sends the render
/// state's visual context tree where `sends_visual_context_tree` says it changed (see [`commit_frame`]).
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. `presentation` must name a presentation the host
/// gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_commit_unrecorded_frame(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    sends_visual_context_tree: bool,
    timestamp: f64,
    presentation: crate::painting::ffi::FfiPresentation,
) {
    let recorder = take_recorder_for_recording(host, read);
    let content = crate::render_state::CommittedContent::Unrecorded {
        sends_visual_context_tree,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { commit_frame(host, read, recorder, content, false, timestamp, presentation) };
}

/// Renders the SVG images of the committed frame of `host`'s document that waits for them, with the callbacks in
/// `publish`, into `resources`, and hands the frame back to the Paint thread, which presents it with them. The frame
/// flies again until the host takes it in.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, whose committed frame landed as
/// [`FfiRecordingLanding::NeedsVectorImages`]; the callbacks in `publish` are called synchronously with their context,
/// which adds what they render to `resources`, a `Web::Compositor::VectorImageResources` the host gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_render_vector_images(
    host: &crate::render_state::DocumentHost,
    publish: crate::painting::host::FfiRecordingPublishCallbacks,
    resources: std::ptr::NonNull<c_void>,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let resources = unsafe { crate::painting::presentation::VectorImageResources::adopt(resources) };
    let (frame, recorder) = host
        .recording()
        .take_vector_image_frame()
        .expect("a committed frame waits for its SVG images");
    let display_list_ids =
        crate::painting::record::publish::render_vector_images(frame.get().requests(), &main_thread, &publish);
    let flight = crate::paint_stage::submit_presenting(frame, move |frame, presenting| {
        frame
            .into_inner()
            .present(recorder, &display_list_ids, resources, presenting)
    });
    host.recording().fly(flight);
}

/// Holds the next recording that flies before it reads its frame, until the test releases it or the host waits for it.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_hold_next_recording_for_testing() {
    crate::painting::recording_slot::hold_next_recording_for_testing();
}

/// Lets the recording held for the test go.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_release_held_recording_for_testing() {
    crate::painting::recording_slot::release_held_recording_for_testing();
}

/// Takes the recording in flight of `host`'s document in where it has finished, and answers how it landed. The event
/// loop calls it between two tasks, so it never waits. A recording that landed gives back the presentation it took, if
/// any, through `presentation`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the event loop must call this between two tasks.
/// `presentation` must be valid for writes; the host takes over what it names.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_take_finished_recording_in(
    host: &crate::render_state::DocumentHost,
    presentation: *mut crate::painting::ffi::FfiPresentation,
) -> FfiRecordingLanding {
    let boundary = crate::render_state::TaskBoundary::at_event_loop_entry(&TAKES_FINISHED_RECORDING_IN);
    let landing = host.recording().take_finished_recording_in(
        &boundary,
        |rows_version| landed_recording_stands(host, rows_version),
        &mut |output, hit_test_list_changed, publishes_recording| {
            take_in_presented_recording(host, output, hit_test_list_changed, publishes_recording);
        },
    );
    // SAFETY: As above.
    unsafe { recording_landing(landing, presentation) }
}

/// Waits for the recording in flight of `host`'s document and takes it in, and answers how it landed (see
/// [`render_state_take_finished_recording_in`]).
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. `presentation` must be valid for writes; the host
/// takes over what it names.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_join_recording_in_flight(
    host: &crate::render_state::DocumentHost,
    presentation: *mut crate::painting::ffi::FfiPresentation,
) -> FfiRecordingLanding {
    let landing = host.recording().join_recording_in_flight(
        |rows_version| landed_recording_stands(host, rows_version),
        &mut |output, hit_test_list_changed, publishes_recording| {
            take_in_presented_recording(host, output, hit_test_list_changed, publishes_recording);
        },
    );
    // SAFETY: As above.
    unsafe { recording_landing(landing, presentation) }
}

/// Takes the output of a recording that presented itself in as the document's last.
fn take_in_presented_recording(
    host: &crate::render_state::DocumentHost,
    output: std::sync::Arc<crate::painting::record::RecordingOutput>,
    hit_test_list_changed: bool,
    publishes_recording: bool,
) {
    host.queue_change(crate::render_state::ArenaChange::Paint(
        crate::painting::paint_changes::PaintChange::TakeInRecording {
            output,
            hit_test_list_changed,
            publishes_recording,
        },
    ));
}

/// Answers how a recording landed, and writes the presentation it gave back to `presentation`.
///
/// # Safety
///
/// `presentation` must be valid for writes.
unsafe fn recording_landing(
    landing: crate::painting::recording_slot::RecordingLanding,
    presentation: *mut crate::painting::ffi::FfiPresentation,
) -> FfiRecordingLanding {
    use crate::painting::recording_slot::RecordingLanding;
    match landing {
        RecordingLanding::NoneInFlight => FfiRecordingLanding::NoneInFlight,
        RecordingLanding::StillInFlight => FfiRecordingLanding::StillInFlight,
        RecordingLanding::Landed(given_back) => {
            // SAFETY: Guaranteed by the caller.
            unsafe { give_back(given_back, presentation) };
            FfiRecordingLanding::Landed
        }
        RecordingLanding::LandedBehindRows(given_back) => {
            // SAFETY: Guaranteed by the caller.
            unsafe { give_back(given_back, presentation) };
            FfiRecordingLanding::LandedBehindRows
        }
        RecordingLanding::NothingRecorded(given_back) => {
            // SAFETY: Guaranteed by the caller.
            unsafe { give_back(given_back, presentation) };
            FfiRecordingLanding::NothingRecorded
        }
        RecordingLanding::PresentedUnrecorded(given_back) => {
            // SAFETY: Guaranteed by the caller.
            unsafe { give_back(given_back, presentation) };
            FfiRecordingLanding::PresentedUnrecorded
        }
        RecordingLanding::NeedsVectorImages => FfiRecordingLanding::NeedsVectorImages,
    }
}

/// Writes the presentation a recording gave back, if any, to `presentation`.
///
/// # Safety
///
/// `presentation` must be valid for writes.
unsafe fn give_back(
    given_back: Option<crate::painting::presentation::Presentation>,
    presentation: *mut crate::painting::ffi::FfiPresentation,
) {
    let given_back = given_back.map_or_else(Default::default, crate::painting::presentation::Presentation::into_ffi);
    // SAFETY: Guaranteed by the caller.
    unsafe { presentation.write(given_back) };
}

/// Whether the rows of `host`'s document are still at `version`, so that the hit-test list of a recording that landed
/// with a frame frozen there still stands for it. The recording dropped its frame and the lease it held, so the style
/// engine frees what it kept for that lease here too, however idle the document stays.
fn landed_recording_stands(host: &crate::render_state::DocumentHost, version: crate::layout::RowsVersion) -> bool {
    // A frame in flight, or a round that flew and is not paid yet, writes the rows, which then stand for no recording
    // made before it.
    let Some(here) = host.layout_waits_for_no_frame() else {
        return false;
    };
    host.ask(here, |state| {
        let arena = state.arena_mut();
        arena.with_style_engine(|engine| engine.free_style_records_kept_for_leases());
        arena.rows_version() == version
    })
}

/// Answers `query` from the hit-test list of the last recording `host`'s document published, built up by `build`, and
/// the rows as of every write the host made, with every row's overflow measured, in `read`. Answers `default` where no recording
/// was published. Every entry that hits tests is called with a live document host, on its document's thread.
fn with_hit_test_list<R>(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    default: R,
    build: impl FnOnce(&mut crate::painting::hit_test::HitTestList, &crate::painting::paint_read::PaintSource<'_>),
    query: impl FnOnce(
        &crate::painting::hit_test::HitTestList,
        &crate::layout::row_reads::RowSnapshot,
        &crate::painting::paint_read::PaintSource<'_>,
    ) -> R,
) -> R {
    // SAFETY: The host is live.
    let rows = host.fresh_measured_rows(read);
    let absolute_rects = std::cell::RefCell::default();
    let source = crate::painting::paint_read::PaintSource::over_rows(&rows.paintable, &absolute_rects);
    let mut recording = host.recording();
    let Some(list) = recording.hit_test_list().as_mut() else {
        return default;
    };
    build(list, &source);
    query(list, &rows, &source)
}

fn with_hit_test_list_items_only<R>(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::painting::paint_read::PaintSource<'_>) -> R,
) -> R {
    with_hit_test_list(host, read, default, |_, _| {}, |list, _, source| query(list, source))
}

fn with_hit_test_list_and_caret_lines<R>(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::painting::paint_read::PaintSource<'_>) -> R,
) -> R {
    with_hit_test_list(
        host,
        read,
        default,
        |list, source| list.build_caret_lines_if_needed(source),
        |list, _, source| query(list, source),
    )
}

fn with_hit_test_list_spatial_indexes_and_visual_context_tree<R>(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    needs_caret_lines: bool,
    default: R,
    query: impl FnOnce(
        &crate::painting::hit_test::HitTestList,
        &crate::painting::visual_context::VisualContextTree,
        &crate::painting::paint_read::PaintSource<'_>,
    ) -> R,
) -> R {
    with_hit_test_list(
        host,
        read,
        None,
        |list, source| {
            list.build_spatial_indexes_if_needed();
            if needs_caret_lines {
                list.build_caret_lines_if_needed(source);
            }
        },
        |list, rows, source| {
            let tree = rows.visual_context_tree.as_deref()?;
            Some(query(list, tree, source))
        },
    )
    .unwrap_or(default)
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
/// `host` must be a live document host, on its document's thread; the
/// sink pointer must stay valid for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_visit_chrome_widgets(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    sink: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, NodeSlotId, u8),
) {
    with_hit_test_list_items_only(host, read, (), |list, _arena| {
        for item in list.items.iter() {
            if item.chrome_widget_kind == crate::painting::hit_test::CHROME_WIDGET_NONE {
                continue;
            }
            // SAFETY: The C++ host consumes the visit synchronously.
            unsafe { visit(sink, item.paintable, item.chrome_widget_kind) };
        }
    });
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_list_generation(host: &crate::render_state::DocumentHost) -> u64 {
    let mut recording = host.recording();
    recording.hit_test_list().as_ref().map_or(0, |list| list.generation)
}

/// The DOM node a hit on the paintable dispatches events to.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_paintable_event_dispatch_target(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    slot: NodeSlotId,
) -> crate::painting::host::FfiNodeIdentity {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_measured_rows(host, read, |rows| {
            crate::painting::hit_test::resolve::event_dispatch_target_of_paintable(rows, slot)
        })
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread;
/// `index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_item_facts(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    index: usize,
) -> crate::painting::host::FfiHitTestItemExport {
    with_hit_test_list_items_only(host, read, None, |list, arena| {
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
            caret_rect: item.caret_rect.into(),
            context: item.context,
        })
    })
    .expect("no hit-test list")
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_resolve_hit(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    item_index: usize,
    local_point: FfiCssPixelPoint,
) -> crate::painting::host::FfiResolvedHit {
    with_hit_test_list_items_only(host, read, Default::default(), |list, arena| {
        list.resolve_hit(arena, item_index, local_point.into())
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_find_topmost_item(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
) -> crate::painting::host::FfiTopmostItem {
    with_hit_test_list_spatial_indexes_and_visual_context_tree(
        host,
        read,
        false,
        Default::default(),
        |list, tree, arena| ffi_topmost(list.find_topmost_item(arena, tree, &callbacks, point.into())),
    )
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_all(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
    push_context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, usize),
) {
    let indices = with_hit_test_list_spatial_indexes_and_visual_context_tree(
        host,
        read,
        false,
        Vec::new(),
        |list, tree, arena| list.hit_test_all(arena, tree, &callbacks, point.into()),
    );
    for index in indices {
        // SAFETY: The C++ sink consumes the index synchronously.
        unsafe { push(push_context, index) };
    }
}

/// The caret position at `point`, constrained to `constraint_scope` when it names a node.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread; the callbacks must stay valid for this synchronous
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_position_from_point(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
    mode: u8,
    constraint_scope: crate::painting::host::FfiNodeIdentity,
) -> FfiCaretAt {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    let mode = crate::painting::hit_test::caret::CaretPositionMode::from_u8(mode);
    with_hit_test_list_spatial_indexes_and_visual_context_tree(host, read, true, None, |list, tree, arena| {
        list.caret_position_from_point(
            &main_thread,
            arena,
            tree,
            &callbacks,
            point.into(),
            mode,
            constraint_scope,
        )
    })
    .unwrap_or_else(FfiCaretAt::none)
}

/// The caret position at the start (`edge` 1) or end (`edge` 2) of the painted line holding the position `query`
/// describes.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread; the query must stay valid for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_at_line_edge(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    query: crate::painting::host::FfiCaretPositionQuery,
    offset: usize,
    affinity_is_downstream: bool,
    edge: u8,
) -> FfiCaretAt {
    let edge = crate::painting::hit_test::caret::CaretPositionType::from_u8(edge);
    with_hit_test_list_and_caret_lines(host, read, None, |list, arena| {
        list.caret_at_line_edge(arena, &query, offset, affinity_is_downstream, edge)
    })
    .unwrap_or_else(FfiCaretAt::none)
}

/// The caret position on the line visually after (`direction` 1) or before (`direction` 0) the line holding the
/// position `query` describes, among the lines inside `scope`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread; the query and callbacks must stay valid for this
/// synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_on_adjacent_line(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    query: crate::painting::host::FfiCaretPositionQuery,
    offset: usize,
    affinity_is_downstream: bool,
    direction: u8,
    inline_coordinate: i32,
    scope: crate::painting::host::FfiNodeIdentity,
) -> FfiCaretAt {
    let direction = if direction == 1 {
        crate::painting::hit_test::caret::CaretLineDirection::Next
    } else {
        crate::painting::hit_test::caret::CaretLineDirection::Previous
    };
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    with_hit_test_list_and_caret_lines(host, read, None, |list, arena| {
        list.caret_on_adjacent_line(
            &main_thread,
            arena,
            &callbacks,
            &query,
            offset,
            affinity_is_downstream,
            direction,
            CssPixels::from_raw(inline_coordinate),
            scope,
        )
    })
    .unwrap_or_else(FfiCaretAt::none)
}

/// The block-axis middle of the painted line holding the position `query` describes, if a line holds it.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread; the query must stay valid for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_line_block_coordinate(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    query: crate::painting::host::FfiCaretPositionQuery,
    offset: usize,
    affinity_is_downstream: bool,
    out_coordinate: &mut i32,
) -> bool {
    let coordinate = with_hit_test_list_and_caret_lines(host, read, None, |list, arena| {
        let line_index = list.caret_line_for_position(arena, &query, offset, affinity_is_downstream)?;
        Some(list.line_block_coordinate(line_index))
    });
    let Some(coordinate) = coordinate else {
        return false;
    };
    *out_coordinate = coordinate.raw_value();
    true
}

/// The style-tree identity of the first `<area>` of the image's map, in tree order, whose shape
/// covers the point. Zero when the image has no map, or when no shape covers the point.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_image_map_area_for_point(
    host: &crate::render_state::DocumentHost,
    slot: NodeSlotId,
    x: f32,
    y: f32,
) -> u32 {
    host.fresh_rows(super::node_read()).image_map_area_for_point(slot, x, y)
}
