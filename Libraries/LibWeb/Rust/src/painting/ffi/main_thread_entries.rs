/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;
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
    viewport_overflow_x: u8,
    viewport_overflow_y: u8,
    enlarged: bool,
    has_device_scroll_offset: bool,
    device_scroll_offset: f32,
    device_pixels_per_css_pixel: f64,
) -> FfiOptionalScrollbarData {
    // SAFETY: Guaranteed by the caller.
    let data = unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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
    viewport_overflow_x: u8,
    viewport_overflow_y: u8,
) -> FfiPhysicalResizeAxes {
    // SAFETY: Guaranteed by the caller.
    let axes = unsafe {
        read_measured_rows(host, super::node_read(), |rows| {
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_scrolling_box_for_scroll_step(
    host: &crate::render_state::DocumentHost,
    target: NodeSlotId,
    viewport: NodeSlotId,
    delta: FfiCssPixelPoint,
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
) -> NodeSlotId {
    let overflow = ViewportWheelOverflow {
        x: viewport_wheel_overflow_x,
        y: viewport_wheel_overflow_y,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(
            host,
            super::node_read(),
            (target, viewport, delta, overflow),
            |arena, (target, viewport, delta, overflow)| {
                arena.measure_scrollable_overflow();
                crate::painting::scroll_chain::scrolling_box_for_scroll_step(
                    &arena.paintable_rows(),
                    target,
                    viewport,
                    delta.into(),
                    overflow,
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
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
    context: *mut c_void,
    push_scrollable_box: unsafe extern "C" fn(*mut c_void, NodeSlotId, f64, f64),
) {
    let overflow = ViewportWheelOverflow {
        x: viewport_wheel_overflow_x,
        y: viewport_wheel_overflow_y,
    };
    // SAFETY: Guaranteed by the caller.
    let boxes = unsafe {
        read_arena(
            host,
            super::node_read(),
            (start, wheel_delta_x, wheel_delta_y, overflow),
            |arena, (start, wheel_delta_x, wheel_delta_y, overflow)| {
                arena.measure_scrollable_overflow();
                let mut boxes = Vec::new();
                crate::painting::scroll_chain::for_each_wheel_scrollable_box_in_containing_block_chain(
                    &arena.paintable_rows(),
                    start,
                    wheel_delta_x,
                    wheel_delta_y,
                    overflow,
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
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
) -> NodeSlotId {
    let overflow = ViewportWheelOverflow {
        x: viewport_wheel_overflow_x,
        y: viewport_wheel_overflow_y,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), (start, overflow), |arena, (start, overflow)| {
            arena.measure_scrollable_overflow();
            crate::painting::scroll_chain::first_wheel_scrollable_box_in_containing_block_chain(
                &arena.paintable_rows(),
                start,
                overflow,
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
        pending.prepare(arena, visual_context_update_pending, &inputs)
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

/// Records `host`'s document's viewport with `inputs` on the Paint thread: in step with the host, or beside the event
/// loop where `blocker` is none, until the host takes it in. A recording that flies takes the presentation `presentation`
/// names, if any, to present its frame with, and nulls it there; any other leaves it with the host. Answers how the
/// recording started, which it does not where the viewport has no box to record.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. Input arrays and byte buffers must be valid and
/// immutable for this call; fonts for enabled overlays must be live `Gfx::Font`s. `presentation` must be valid for
/// reads and writes, and name a presentation the host gives up where the recording takes it, or none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_record_display_list(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiRecordingInputs,
    blocker: FfiFlightBlocker,
    presentation: *mut crate::painting::ffi::FfiPresentation,
) -> FfiRecordingStart {
    // The recording takes the recorder state, which a clock lease brings back.
    host.end_clock_lease_waiting(read);
    let mut recording = host.recording();
    debug_assert!(
        !recording.has_pending_recording() && !recording.has_recording_in_flight(),
        "a recording must be published before the next one starts"
    );
    recording.discard_pending_recording();
    let recorder = recording.take_recorder();
    let frame_inputs = crate::painting::recording_slot::FrameInputs {
        viewport,
        css_viewport_rect: inputs.css_viewport_rect.into(),
        publishes_recording: inputs.publishes_recording,
        published_root_background_canvas_rect: recorder
            .published_recording
            .as_ref()
            .map(|recording| recording.root_background_canvas_rect),
        hit_test_item_capacity_hint: recording.hit_test_item_capacity_hint(),
    };
    // SAFETY: As above.
    let Some(frame) = (unsafe {
        read_arena(
            host,
            read,
            frame_inputs,
            crate::painting::recording_slot::freeze_recording_frame,
        )
    }) else {
        recording.give_back_recorder(recorder);
        return FfiRecordingStart::NothingToRecord;
    };
    // SAFETY: The host lends the input arrays and buffers for this call, and the inputs copy what they read of them.
    let inputs = unsafe { inputs.recording_inputs(frame.tree_inputs, frame.root_background_source) };
    let job =
        crate::painting::recording_slot::RecordingJob::new(frame.frame, recorder, viewport, frame.trace_recordings);
    match crate::painting::recording_slot::FlightLicense::for_blocker(blocker) {
        Some(license) => {
            // SAFETY: Guaranteed by the caller.
            let presentation = unsafe { crate::painting::presentation::Presentation::take(&mut *presentation) };
            recording.fly(job.fly(inputs, presentation, license), frame.rows_version);
            FfiRecordingStart::InFlight
        }
        None => {
            let answer = job.run_on_paint_thread(inputs);
            recording.accept_recording_answer(answer);
            FfiRecordingStart::Recorded
        }
    }
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
/// `host` must be a live document host, on its document's thread;
/// the callback context and function pointers must remain valid for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_line_for_position(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    query: crate::painting::host::FfiCaretPositionQuery,
    offset: usize,
    affinity_is_downstream: bool,
) -> crate::painting::host::FfiCaretLineForPosition {
    with_hit_test_list_and_caret_lines(host, read, Default::default(), |list, arena| {
        match list.caret_line_for_position(arena, &query, offset, affinity_is_downstream) {
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
/// `host` must be a live document host, on its document's thread; `line_index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_line(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    line_index: usize,
) -> crate::painting::host::FfiCaretLineExport {
    with_hit_test_list_and_caret_lines(host, read, Default::default(), |list, _| {
        let line = &list.caret_lines[line_index];
        crate::painting::host::FfiCaretLineExport {
            rect: line.rect.into(),
            context: line.context,
            first_caret_item_index: line.first_caret_item_index,
            last_caret_item_index: line.last_caret_item_index,
        }
    })
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

/// The DOM node the hit-test item stands for.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_item_target(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    item_index: usize,
) -> crate::painting::host::FfiNodeIdentity {
    with_hit_test_list_items_only(host, read, Default::default(), |list, arena| {
        list.item_target(arena, item_index)
    })
}

/// The DOM node the hit-test item dispatches events to.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_item_dispatch_target(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    item_index: usize,
) -> crate::painting::host::FfiNodeIdentity {
    with_hit_test_list_items_only(host, read, Default::default(), |list, arena| {
        list.item_dispatch_target(arena, item_index)
    })
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
/// `host` must be a live document host, on its document's thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_resolve_caret(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    item_index: usize,
    local_point: FfiCssPixelPoint,
    position_type: u8,
) -> crate::painting::host::FfiResolvedCaret {
    with_hit_test_list_items_only(host, read, Default::default(), |list, arena| {
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
pub unsafe extern "C" fn layout_hit_test_find_topmost_items_for_caret(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
) -> crate::painting::host::FfiTopmostItemsForCaret {
    with_hit_test_list_spatial_indexes_and_visual_context_tree(
        host,
        read,
        false,
        Default::default(),
        |list, tree, arena| {
            let (caret_item, hit_item) = list.find_topmost_items_for_caret(arena, tree, &callbacks, point.into());
            crate::painting::host::FfiTopmostItemsForCaret {
                caret_item: ffi_topmost(caret_item),
                hit_item: ffi_topmost(hit_item),
            }
        },
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_item_at_line_edge(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    line_index: usize,
    position_type: u8,
) -> usize {
    let position_type = crate::painting::hit_test::caret::CaretPositionType::from_u8(position_type);
    with_hit_test_list_and_caret_lines(host, read, usize::MAX, |list, _| {
        list.item_at_line_edge(line_index, position_type)
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_caret_item_for_line(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    line_index: usize,
    point: FfiCssPixelPoint,
    mode: u8,
) -> crate::painting::host::FfiCaretItemForLine {
    with_hit_test_list_and_caret_lines(host, read, Default::default(), |list, arena| {
        match list.caret_item_for_line(
            arena,
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
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_line_block_coordinate(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    line_index: usize,
) -> i32 {
    with_hit_test_list_and_caret_lines(host, read, 0, |list, _| {
        list.line_block_coordinate(line_index).raw_value()
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_item_is_inline_adjacent_to_line(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    item_index: usize,
    line_index: usize,
) -> bool {
    with_hit_test_list_and_caret_lines(host, read, false, |list, _| {
        list.item_is_inline_adjacent_to_line(item_index, line_index)
    })
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

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_find_closest_line(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
    mode: u8,
    scoped: bool,
    respect_clip: bool,
) -> crate::painting::host::FfiClosestLine {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    with_hit_test_list_spatial_indexes_and_visual_context_tree(
        host,
        read,
        true,
        Default::default(),
        |list, tree, arena| {
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
        },
    )
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_hit_test_adjacent_line(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
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
    let main_thread = unsafe { main_thread(host) };
    with_hit_test_list_and_caret_lines(host, read, Default::default(), |list, arena| {
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
