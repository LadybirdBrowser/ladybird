/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points of the compositor's logic for the C++ of the Compositor around it, which calls them from its event
//! loop only.

use crate::core::messages::{MessagesFromPresentation, MessagesFromWebContent};
use crate::core::presenting::{GpuBackend, PresentationCapabilities};
use crate::core::{CompositorCore, ContextOwnerCheckResult, VisualContextTreeUpdateResult};
use crate::host::{ContextTimer, FfiCompositorCoreHost, Host, PresentationId, WebContentClientId};
use crate::overlays::{
    PausedDebuggerOverlayAction, PausedDebuggerOverlayState, PictureInPictureControl, PictureInPictureControlsState,
    UIOverlays,
};
use crate::raster_host::{FfiRasterHost, RasterHost};
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ffi::{FfiDisplayList, tree_sharing_handle};
use libcompositing_rust::ipc_types::{
    Compositing_ContextVisibility as ContextVisibility, compositor_presentation_server, compositor_web_content_server,
};
use libcompositing_rust::scrolling::{KeyboardScrollState, ScrollStateSnapshot};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{IntRect, IntSize};
use libipc_rust::cpp_connection::{EncodedValue, RawMessage};
use std::ffi::c_void;
use std::rc::Rc;

fn time(nanoseconds: i64) -> MonotonicTime {
    MonotonicTime::from_nanoseconds(nanoseconds)
}

/// # Safety
///
/// `host.host` and `raster_host.host` must outlive the core, and none of their entry points may call back into the
/// core.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_create(
    host: FfiCompositorCoreHost,
    raster_host: FfiRasterHost,
    scrollbars_always_shown: bool,
    gpu: GpuBackend,
) -> *mut CompositorCore {
    // SAFETY: The caller keeps the hosts alive for as long as the core, and their entry points out of the core.
    let (host, raster_host) = unsafe { (Rc::new(Host::new(host)), RasterHost::new(raster_host)) };
    Box::into_raw(Box::new(CompositorCore::new(
        host,
        raster_host,
        scrollbars_always_shown,
        gpu,
    )))
}

/// # Safety
///
/// `core` must be a core from `compositor_core_create` that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_destroy(core: *mut CompositorCore) {
    // SAFETY: The caller hands over the core `compositor_core_create` made.
    drop(unsafe { Box::from_raw(core) });
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_create_context(
    core: &mut CompositorCore,
    context_id: u64,
    has_page_id: bool,
    page_id: u64,
    web_content_client_id: u64,
    now_nanoseconds: i64,
) {
    core.create_context(
        CompositorContextId(context_id),
        has_page_id.then_some(page_id),
        WebContentClientId(web_content_client_id),
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_destroy_context(core: &mut CompositorCore, context_id: u64, now_nanoseconds: i64) {
    core.destroy_context(CompositorContextId(context_id), time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_context_can_be_nested_in(
    core: &CompositorCore,
    context_id: u64,
    parent_context_id: u64,
) -> bool {
    core.context_can_be_nested_in(CompositorContextId(context_id), CompositorContextId(parent_context_id))
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_presents_to_client(core: &CompositorCore, context_id: u64) -> bool {
    core.presents_to_client(CompositorContextId(context_id))
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_context_is_effectively_visible(core: &CompositorCore, context_id: u64) -> bool {
    core.context_is_effectively_visible(CompositorContextId(context_id))
}

/// Answers whether the context is nested in another, which is written through `parent_context_id`.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_parent_context(
    core: &CompositorCore,
    context_id: u64,
    parent_context_id: &mut u64,
) -> bool {
    let Some(parent) = core.parent_context(CompositorContextId(context_id)) else {
        return false;
    };
    *parent_context_id = parent.0;
    true
}

/// Answers whether the context is bound to a presentation, which is written through `presentation_id`.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_bound_presentation(
    core: &CompositorCore,
    context_id: u64,
    presentation_id: &mut u64,
) -> bool {
    let Some(bound_presentation) = core.bound_presentation(CompositorContextId(context_id)) else {
        return false;
    };
    *presentation_id = bound_presentation.0;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_parent_context(
    core: &CompositorCore,
    context_id: u64,
    has_parent_context: bool,
    parent_context_id: u64,
    now_nanoseconds: i64,
) {
    core.set_parent_context(
        CompositorContextId(context_id),
        has_parent_context.then_some(CompositorContextId(parent_context_id)),
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_stop_presenting_to_client(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
) {
    core.stop_presenting_to_client(CompositorContextId(context_id), time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_viewport_size_updated(
    core: &CompositorCore,
    context_id: u64,
    viewport_size: IntSize,
    device_pixel_ratio: f64,
    window_resize_in_progress: bool,
    now_nanoseconds: i64,
) {
    core.viewport_size_updated(
        CompositorContextId(context_id),
        viewport_size,
        device_pixel_ratio,
        window_resize_in_progress,
        time(now_nanoseconds),
    );
}

/// What a presentation makes of the frames and the scrolling of the contexts it shows.
#[repr(C)]
pub struct FfiPresentationCapabilities {
    pub synthesizes_scroll_momentum: bool,
    pub scrolls_elastically: bool,
    pub scrollbar_track_press_jumps: bool,
    pub has_gpu_presentation_adapter: bool,
    pub gpu_presentation_adapter_luid: u64,
    pub cannot_present_gpu_images: bool,
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_register_presentation(
    core: &CompositorCore,
    presentation_id: u64,
    capabilities: &FfiPresentationCapabilities,
) {
    core.register_presentation(
        PresentationId(presentation_id),
        PresentationCapabilities {
            synthesizes_scroll_momentum: capabilities.synthesizes_scroll_momentum,
            scrolls_elastically: capabilities.scrolls_elastically,
            scrollbar_track_press_jumps: capabilities.scrollbar_track_press_jumps,
            gpu_presentation_adapter_luid: capabilities
                .has_gpu_presentation_adapter
                .then_some(capabilities.gpu_presentation_adapter_luid),
            cannot_present_gpu_images: capabilities.cannot_present_gpu_images,
        },
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_unregister_presentation(
    core: &CompositorCore,
    presentation_id: u64,
    now_nanoseconds: i64,
) {
    core.unregister_presentation(PresentationId(presentation_id), time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_presentation_gpu_capability(
    core: &CompositorCore,
    presentation_id: u64,
    supported: bool,
    adapter_luid: u64,
    now_nanoseconds: i64,
) {
    core.set_presentation_gpu_capability(
        PresentationId(presentation_id),
        supported,
        adapter_luid,
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_scrollbar_track_press_jumps(
    core: &CompositorCore,
    presentation_id: u64,
    jumps: bool,
) {
    core.set_scrollbar_track_press_jumps(PresentationId(presentation_id), jumps);
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_bind_presentation(
    core: &CompositorCore,
    context_id: u64,
    presentation_id: u64,
    now_nanoseconds: i64,
) {
    core.bind_presentation(
        CompositorContextId(context_id),
        PresentationId(presentation_id),
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_shown_in_remote_layer(
    core: &CompositorCore,
    context_id: u64,
    shown: bool,
    now_nanoseconds: i64,
) {
    core.set_shown_in_remote_layer(CompositorContextId(context_id), shown, time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_display_metadata(
    core: &CompositorCore,
    context_id: u64,
    has_display_id: bool,
    display_id: u64,
    refresh_rate: f64,
    now_nanoseconds: i64,
) {
    core.set_display_metadata(
        CompositorContextId(context_id),
        has_display_id.then_some(display_id),
        refresh_rate,
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_window_server_may_show_high_dynamic_range(
    core: &CompositorCore,
    may_show: bool,
    now_nanoseconds: i64,
) {
    core.set_window_server_may_show_high_dynamic_range(may_show, time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_presents_from_cpu_memory(
    core: &CompositorCore,
    context_id: u64,
    presents_from_cpu_memory: bool,
    now_nanoseconds: i64,
) {
    core.set_presents_from_cpu_memory(
        CompositorContextId(context_id),
        presents_from_cpu_memory,
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_request_rendering_opportunity(
    core: &CompositorCore,
    context_id: u64,
    maximum_frames_per_second: f64,
    now_nanoseconds: i64,
) {
    core.request_rendering_opportunity(
        CompositorContextId(context_id),
        maximum_frames_per_second,
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_request_clock_tick(
    core: &CompositorCore,
    context_id: u64,
    maximum_frames_per_second: f64,
) {
    core.request_clock_tick(CompositorContextId(context_id), maximum_frames_per_second);
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_hurry_rendering_opportunity(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
) {
    core.hurry_rendering_opportunity(CompositorContextId(context_id), time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_present_frame_of_page(
    core: &CompositorCore,
    context_id: u64,
    viewport_rect: IntRect,
    now_nanoseconds: i64,
) {
    core.present_frame_of_page(CompositorContextId(context_id), viewport_rect, time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_presented_store_released(
    core: &CompositorCore,
    context_id: u64,
    bitmap_id: i32,
    now_nanoseconds: i64,
) {
    core.presented_store_released(CompositorContextId(context_id), bitmap_id, time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_vsync(
    core: &CompositorCore,
    has_display_id: bool,
    display_id: u64,
    frame_time_nanoseconds: i64,
    now_nanoseconds: i64,
) {
    core.vsync(
        has_display_id.then_some(display_id),
        time(frame_time_nanoseconds),
        time(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_gpu_work_finished(core: &CompositorCore, token: u64, now_nanoseconds: i64) {
    core.gpu_work_finished(token, time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_frame_ready_for_hand_off(core: &CompositorCore, token: u64, now_nanoseconds: i64) {
    core.frame_ready_for_hand_off(token, time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_context_content_changed(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
) {
    core.context_content_changed(CompositorContextId(context_id), time(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_canvas_content_changed(
    core: &CompositorCore,
    web_content_client_id: u64,
    canvas_id: u64,
    now_nanoseconds: i64,
) {
    core.canvas_content_changed(
        WebContentClientId(web_content_client_id),
        canvas_id,
        time(now_nanoseconds),
    );
}

/// Paints a screenshot of the context into the surface, answering false for a context with nothing to paint yet.
///
/// # Safety
///
/// `surface` must be a live surface, which the call lends.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_paint_screenshot(
    core: &CompositorCore,
    context_id: u64,
    surface: *mut c_void,
    now_nanoseconds: i64,
) -> bool {
    let Some(surface) = core.raster().retain_surface(surface) else {
        return false;
    };
    core.paint_screenshot(CompositorContextId(context_id), &surface, time(now_nanoseconds))
}

/// The surface that holds the frame on screen as a whole, with one reference that the caller takes over, and the size
/// of the frame in it. Answers false where no frame is on screen.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_presented_frame_for_snapshot(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
    surface: &mut *mut c_void,
    frame_size: &mut IntSize,
) -> bool {
    let Some((source, size)) =
        core.presented_frame_for_snapshot(CompositorContextId(context_id), time(now_nanoseconds))
    else {
        return false;
    };
    *surface = core.raster().leak_surface(source);
    *frame_size = size;
    true
}

/// Takes a message of the CompositorWebContentServer endpoint that is marked [RustReceiver].
///
/// # Safety
///
/// `message` must be one `IPC::RawMessage::hand_to_rust()` made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_handle_web_content_message(
    core: &CompositorCore,
    web_content_client_id: u64,
    message: RawMessage<'_>,
    now_nanoseconds: i64,
) {
    let mut stub = MessagesFromWebContent {
        core,
        client_id: WebContentClientId(web_content_client_id),
        now: time(now_nanoseconds),
    };
    // SAFETY: The caller hands over a message of that endpoint.
    unsafe {
        message.hand_to(|bytes, attachments| compositor_web_content_server::dispatch(&mut stub, bytes, attachments));
    };
    core.finish_message(time(now_nanoseconds));
}

/// Takes a message of the CompositorPresentationServer endpoint that is marked [RustReceiver].
///
/// # Safety
///
/// `message` must be one `IPC::RawMessage::hand_to_rust()` made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_handle_presentation_message(
    core: &CompositorCore,
    presentation_id: u64,
    message: RawMessage<'_>,
    now_nanoseconds: i64,
) {
    let mut stub = MessagesFromPresentation {
        core,
        presentation_id: PresentationId(presentation_id),
        now: time(now_nanoseconds),
    };
    // SAFETY: The caller hands over a message of that endpoint.
    unsafe {
        message.hand_to(|bytes, attachments| compositor_presentation_server::dispatch(&mut stub, bytes, attachments));
    };
    core.finish_message(time(now_nanoseconds));
}

/// What the browser draws over a context's page. An action or control is none where it is out of range.
#[repr(C)]
pub struct FfiUIOverlays<'a> {
    pub has_paused_debugger: bool,
    pub paused_debugger_has_font_family: bool,
    pub paused_debugger_font_family: ak::Span<'a, u8>,
    pub paused_debugger_has_hovered_action: bool,
    pub paused_debugger_hovered_action: u8,
    pub has_picture_in_picture_controls: bool,
    pub picture_in_picture_controls_shown: bool,
    pub picture_in_picture_controls_has_hovered_control: bool,
    pub picture_in_picture_controls_hovered_control: u8,
}

/// Answers false, changing nothing, for async scrolling metadata or a scroll state that do not decode, or that hold
/// numbers no scroll can be calculated with.
///
/// # Safety
///
/// The storage of `display_list` must be a live display list storage handle, and `visual_context_tree` a live retained
/// tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_install_display_list_update(
    core: &CompositorCore,
    context_id: u64,
    display_list: &FfiDisplayList<'_>,
    visual_context_tree: *const c_void,
    scroll_state: EncodedValue<'_>,
    now_nanoseconds: i64,
) -> bool {
    // SAFETY: The caller lends a strong reference to the storage.
    let Ok(display_list) = (unsafe { display_list.share() }) else {
        return false;
    };
    let Ok(scroll_state_snapshot) = scroll_state.decode::<ScrollStateSnapshot>() else {
        return false;
    };
    core.install_display_list_update(
        CompositorContextId(context_id),
        Rc::new(display_list),
        // SAFETY: The caller lends a strong reference to the tree.
        unsafe { tree_sharing_handle(visual_context_tree) },
        scroll_state_snapshot,
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    )
}

/// Changes nothing for a scroll state or keyboard scroll state that do not decode, or a scroll state with offsets that
/// are not finite.
///
/// # Safety
///
/// `visual_context_tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_update_visual_context_tree(
    core: &CompositorCore,
    context_id: u64,
    visual_context_tree: *const c_void,
    scroll_state: EncodedValue<'_>,
    keyboard_scroll_state: EncodedValue<'_>,
    now_nanoseconds: i64,
) -> VisualContextTreeUpdateResult {
    let (Ok(scroll_state_snapshot), Ok(keyboard_scroll_state)) = (
        scroll_state.decode::<ScrollStateSnapshot>(),
        keyboard_scroll_state.decode::<KeyboardScrollState>(),
    ) else {
        return VisualContextTreeUpdateResult::InvalidScrollState;
    };
    if !scroll_state_snapshot.has_only_finite_offsets() {
        return VisualContextTreeUpdateResult::InvalidScrollState;
    }
    let tree_was_replaced = core.update_visual_context_tree(
        CompositorContextId(context_id),
        // SAFETY: The caller lends a strong reference to the tree.
        unsafe { tree_sharing_handle(visual_context_tree) },
        scroll_state_snapshot,
        keyboard_scroll_state,
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    );
    if tree_was_replaced {
        VisualContextTreeUpdateResult::TreeReplaced
    } else {
        VisualContextTreeUpdateResult::TreeDropped
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_visibility(
    core: &CompositorCore,
    context_id: u64,
    is_visible: bool,
    now_nanoseconds: i64,
) -> bool {
    let visibility = if is_visible {
        ContextVisibility::Visible
    } else {
        ContextVisibility::Hidden
    };
    core.set_visibility(
        CompositorContextId(context_id),
        visibility,
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_timer_fired(
    core: &CompositorCore,
    context_id: u64,
    timer: ContextTimer,
    now_nanoseconds: i64,
) {
    core.timer_fired(
        CompositorContextId(context_id),
        timer,
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_ui_overlays(
    core: &CompositorCore,
    context_id: u64,
    ui_overlays: &FfiUIOverlays<'_>,
    now_nanoseconds: i64,
) {
    let paused_debugger = ui_overlays.has_paused_debugger.then(|| PausedDebuggerOverlayState {
        font_family: ui_overlays
            .paused_debugger_has_font_family
            .then(|| String::from_utf8_lossy(ui_overlays.paused_debugger_font_family.as_slice()).into_owned()),
        hovered_action: match (
            ui_overlays.paused_debugger_has_hovered_action,
            ui_overlays.paused_debugger_hovered_action,
        ) {
            (true, 0) => Some(PausedDebuggerOverlayAction::StepOver),
            (true, 1) => Some(PausedDebuggerOverlayAction::Continue),
            _ => None,
        },
    });
    let picture_in_picture_controls =
        ui_overlays
            .has_picture_in_picture_controls
            .then_some(PictureInPictureControlsState {
                shown: ui_overlays.picture_in_picture_controls_shown,
                hovered_control: match (
                    ui_overlays.picture_in_picture_controls_has_hovered_control,
                    ui_overlays.picture_in_picture_controls_hovered_control,
                ) {
                    (true, 0) => Some(PictureInPictureControl::BackToTab),
                    (true, 1) => Some(PictureInPictureControl::Close),
                    _ => None,
                },
            });
    core.set_ui_overlays(
        CompositorContextId(context_id),
        UIOverlays {
            paused_debugger,
            picture_in_picture_controls,
        },
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_check_context_owner(
    core: &CompositorCore,
    context_id: u64,
    web_content_client_id: u64,
) -> ContextOwnerCheckResult {
    core.check_context_owner(
        CompositorContextId(context_id),
        WebContentClientId(web_content_client_id),
    )
}
