/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points of the compositor's logic for the C++ of the Compositor around it, which calls them from its event
//! loop only.

use crate::core::messages::{MessagesFromControl, MessagesFromPresentation, MessagesFromWebContent};
use crate::core::presenting::{GpuBackend, PresentationCapabilities};
use crate::core::{CompositorCore, ContextOwnerCheckResult};
use crate::host::{FfiCompositorCoreHost, Host, PresentationId, WebContentClientId};
use crate::raster_host::{FfiRasterHost, RasterHost};
use crate::surfaces::painting_surface::PaintingSurface;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ipc_types::{
    Compositing_ContextVisibility as ContextVisibility, compositor_control_server, compositor_presentation_server,
    compositor_web_content_server, render_clock_server,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::IntSize;
use libipc_rust::cpp_connection::RawMessage;
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
/// `surface` must be a live surface.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_paint_screenshot(
    core: &CompositorCore,
    context_id: u64,
    surface: *const PaintingSurface,
    now_nanoseconds: i64,
) -> bool {
    // SAFETY: The caller lends a live surface, which the painting holds while it lasts.
    let surface = unsafe { PaintingSurface::retained_from_ffi(surface) };
    core.paint_screenshot(CompositorContextId(context_id), &surface, time(now_nanoseconds))
}

/// The surface that holds the frame on screen as a whole, with one reference that the caller takes over, and the size
/// of the frame in it. Answers null where no frame is on screen.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_presented_frame_for_snapshot(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
    frame_size: &mut IntSize,
) -> *const PaintingSurface {
    let Some((source, size)) =
        core.presented_frame_for_snapshot(CompositorContextId(context_id), time(now_nanoseconds))
    else {
        return std::ptr::null();
    };
    *frame_size = size;
    PaintingSurface::into_ffi(source)
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

/// Takes a message of the RenderClockServer endpoint that is marked [RustReceiver], from the render clock channel of a
/// WebContent process.
///
/// # Safety
///
/// `message` must be one `IPC::RawMessage::hand_to_rust()` made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_handle_render_clock_message(
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
        message.hand_to(|bytes, attachments| render_clock_server::dispatch(&mut stub, bytes, attachments));
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

/// Takes a message of the CompositorControlServer endpoint that is marked [RustReceiver].
///
/// # Safety
///
/// `message` must be one `IPC::RawMessage::hand_to_rust()` made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_handle_control_message(
    core: &CompositorCore,
    message: RawMessage<'_>,
    now_nanoseconds: i64,
) {
    let mut stub = MessagesFromControl {
        core,
        now: time(now_nanoseconds),
    };
    // SAFETY: The caller hands over a message of that endpoint.
    unsafe {
        message.hand_to(|bytes, attachments| compositor_control_server::dispatch(&mut stub, bytes, attachments));
    };
    core.finish_message(time(now_nanoseconds));
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
pub extern "C" fn compositor_core_timer_fired(core: &CompositorCore, timer_number: u64, now_nanoseconds: i64) {
    core.timer_fired(timer_number, MonotonicTime::from_nanoseconds(now_nanoseconds));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_forget_web_content_client(core: &CompositorCore, web_content_client_id: u64) {
    let client = WebContentClientId(web_content_client_id);
    core.canvas_2d_contexts().forget_client(client);
    core.webgl_contexts().forget_client(client);
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
