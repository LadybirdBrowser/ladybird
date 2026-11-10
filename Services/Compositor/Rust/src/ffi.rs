/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points of the scroll and input logic for the C++ of the Compositor around it.

use crate::context_state::ScrollCapabilities;
use crate::core::messages::{MessagesFromPresentation, MessagesFromWebContent};
use crate::core::{CompositorCore, ContextOwnerCheckResult};
use crate::frame_inputs::{FrameInputs, ScrollbarOfFrame};
use crate::host::{
    ContextTimer, DisplayTickInputs, DisplayTickResult, FfiCompositorCoreHost, Host, PresentScheduling, PresentationId,
    WebContentClientId,
};
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ffi::{FfiDisplayList, tree_sharing_handle};
use libcompositing_rust::ipc_types::{
    Compositing_ContextVisibility as ContextVisibility, compositor_presentation_server, compositor_web_content_server,
};
use libcompositing_rust::scrolling::wheel_gesture_identity::TouchpadGestureEnd;
use libcompositing_rust::scrolling::{ScrollStateSnapshot, SpatialNodeIndex};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatPoint, IntRect, IntSize};
use libipc_rust::cpp_connection::{EncodedValue, RawMessage};
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

/// # Safety
///
/// `host.host` must outlive the core, and none of the host's entry points may call back into the core.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_create(
    host: FfiCompositorCoreHost,
    scrollbars_always_shown: bool,
) -> *mut CompositorCore {
    // SAFETY: The caller keeps the host alive for as long as the core, and its entry points out of the core.
    let host = Rc::new(unsafe { Host::new(host) });
    Box::into_raw(Box::new(CompositorCore::new(host, scrollbars_always_shown)))
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
) {
    core.create_context(
        CompositorContextId(context_id),
        has_page_id.then_some(page_id),
        WebContentClientId(web_content_client_id),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_destroy_context(core: &mut CompositorCore, context_id: u64) {
    core.destroy_context(CompositorContextId(context_id));
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

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_parent_context(
    core: &CompositorCore,
    context_id: u64,
    has_parent_context: bool,
    parent_context_id: u64,
) {
    core.set_parent_context(
        CompositorContextId(context_id),
        has_parent_context.then_some(CompositorContextId(parent_context_id)),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_stop_presenting_to_client(core: &CompositorCore, context_id: u64) {
    core.stop_presenting_to_client(CompositorContextId(context_id));
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_visibility(
    core: &CompositorCore,
    context_id: u64,
    is_visible: bool,
    now_nanoseconds: i64,
) {
    let visibility = if is_visible {
        ContextVisibility::Visible
    } else {
        ContextVisibility::Hidden
    };
    core.set_visibility(
        CompositorContextId(context_id),
        visibility,
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_viewport_size_updated(
    core: &CompositorCore,
    context_id: u64,
    viewport_size: IntSize,
) {
    core.viewport_size_updated(CompositorContextId(context_id), viewport_size);
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_paused_debugger_overlay_is_shown(
    core: &CompositorCore,
    context_id: u64,
    is_shown: bool,
) {
    core.set_paused_debugger_overlay_is_shown(CompositorContextId(context_id), is_shown);
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_bound_presentation(
    core: &CompositorCore,
    context_id: u64,
    is_bound: bool,
    presentation_id: u64,
) {
    core.set_bound_presentation(
        CompositorContextId(context_id),
        is_bound.then_some(PresentationId(presentation_id)),
    );
}

/// What the presentation a context is shown through makes of the user's scrolling.
#[repr(C)]
pub struct FfiScrollCapabilities {
    pub synthesizes_scroll_momentum: bool,
    pub scrolls_elastically: bool,
    pub scrollbar_track_press_jumps: bool,
    pub touchpad_gestures_end_when_fingers_lift: bool,
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_set_scroll_capabilities(
    core: &CompositorCore,
    context_id: u64,
    capabilities: FfiScrollCapabilities,
) {
    core.set_scroll_capabilities(
        CompositorContextId(context_id),
        &ScrollCapabilities {
            synthesizes_scroll_momentum: capabilities.synthesizes_scroll_momentum,
            scrolls_elastically: capabilities.scrolls_elastically,
            scrollbar_track_press_jumps: capabilities.scrollbar_track_press_jumps,
            touchpad_gesture_end: if capabilities.touchpad_gestures_end_when_fingers_lift {
                TouchpadGestureEnd::WhenFingersLift
            } else {
                TouchpadGestureEnd::AfterIdleDelay
            },
        },
    );
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
    scroll_state_snapshot: EncodedValue<'_>,
    now_nanoseconds: i64,
) -> bool {
    // SAFETY: The caller lends a strong reference to the storage.
    let Ok(display_list) = (unsafe { display_list.share() }) else {
        return false;
    };
    let Ok(scroll_state_snapshot) = scroll_state_snapshot.decode::<ScrollStateSnapshot>() else {
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

/// Answers false, leaving everything as it was, for a tree that does not fit the installed display list.
///
/// # Safety
///
/// `visual_context_tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_update_visual_context_tree(
    core: &CompositorCore,
    context_id: u64,
    visual_context_tree: *const c_void,
    now_nanoseconds: i64,
) -> bool {
    core.update_visual_context_tree(
        CompositorContextId(context_id),
        // SAFETY: The caller lends a strong reference to the tree.
        unsafe { tree_sharing_handle(visual_context_tree) },
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_needs_animation_frames(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
) -> bool {
    core.needs_animation_frames(
        CompositorContextId(context_id),
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    )
}

/// What a frame is painted with besides the display list and its resources, lent for the duration of a call.
#[repr(C)]
pub struct FfiFrameInputs<'a> {
    /// One strong reference to the tree, which the receiver takes over.
    pub visual_context_tree: *const c_void,
    pub presented_device_offsets: ak::Span<'a, FloatPoint>,
    pub scrollbars: ak::Span<'a, ScrollbarOfFrame>,
    pub has_viewport_scroll_node_index: bool,
    pub viewport_scroll_node_index: SpatialNodeIndex,
}

type ReceiveFrameInputs = unsafe extern "C" fn(*mut c_void, &FfiFrameInputs<'_>);

/// # Safety
///
/// `receive` must take over the tree reference and copy what it keeps of the rest.
unsafe fn lend_frame_inputs(frame_inputs: &FrameInputs, receiver: *mut c_void, receive: ReceiveFrameInputs) {
    let lent = FfiFrameInputs {
        visual_context_tree: Arc::into_raw(frame_inputs.visual_context_tree.as_arc().clone()).cast(),
        presented_device_offsets: ak::Span::new(frame_inputs.presented_scroll_state_snapshot.device_offsets()),
        scrollbars: ak::Span::new(&frame_inputs.scrollbars),
        has_viewport_scroll_node_index: frame_inputs.viewport_scroll_node_index.is_some(),
        viewport_scroll_node_index: frame_inputs.viewport_scroll_node_index.unwrap_or_default(),
    };
    // SAFETY: The receiver takes the tree reference over and copies the rest before it returns.
    unsafe { receive(receiver, &lent) };
}

/// Answers false, without calling `receive`, for a context that has no display list yet.
///
/// # Safety
///
/// `receive` must take over the tree reference of the inputs it is lent, copy what it keeps of the rest, and not call
/// back into the core.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_frame_inputs(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
    receiver: *mut c_void,
    receive: ReceiveFrameInputs,
) -> bool {
    let Some(frame_inputs) = core.frame_inputs(
        CompositorContextId(context_id),
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    ) else {
        return false;
    };
    // SAFETY: The caller's receiver does as this function asks of it.
    unsafe { lend_frame_inputs(&frame_inputs, receiver, receive) };
    true
}

/// Answers with the scrollbar fade damage that the screenshot does not present on screen.
///
/// # Safety
///
/// The context must have a display list. `receive` must take over the tree reference of the inputs it is lent, copy
/// what it keeps of the rest, and not call back into the core.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_frame_inputs_for_screenshot(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
    receiver: *mut c_void,
    receive: ReceiveFrameInputs,
    scrollbar_fade_damage_rect: &mut IntRect,
) {
    let screenshot_inputs = core.frame_inputs_for_screenshot(
        CompositorContextId(context_id),
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    );
    // SAFETY: The caller's receiver does as this function asks of it.
    unsafe { lend_frame_inputs(&screenshot_inputs.frame_inputs, receiver, receive) };
    *scrollbar_fade_damage_rect = screenshot_inputs.scrollbar_fade_damage_rect;
}

/// What the presentation knows of a context as a tick of a display begins.
#[repr(C)]
pub struct FfiDisplayTickInputs {
    pub context_is_on_tick_display: bool,
    pub present_is_scheduled_on_tick_display: bool,
    pub ui_overlay_fades_need_frame: bool,
    /// Whether the render clock of the context is due a tick now, of the interval that follows.
    pub clock_tick_is_due: bool,
    pub clock_tick_interval_in_milliseconds: f64,
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_display_tick(
    core: &CompositorCore,
    context_id: u64,
    frame_time_nanoseconds: i64,
    now_nanoseconds: i64,
    inputs: FfiDisplayTickInputs,
) -> DisplayTickResult {
    core.display_tick(
        CompositorContextId(context_id),
        MonotonicTime::from_nanoseconds(frame_time_nanoseconds),
        MonotonicTime::from_nanoseconds(now_nanoseconds),
        &DisplayTickInputs {
            context_is_on_tick_display: inputs.context_is_on_tick_display,
            present_is_scheduled_on_tick_display: inputs.present_is_scheduled_on_tick_display,
            ui_overlay_fades_need_frame: inputs.ui_overlay_fades_need_frame,
            clock_tick_interval_in_milliseconds: inputs
                .clock_tick_is_due
                .then_some(inputs.clock_tick_interval_in_milliseconds),
        },
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

/// A frame that the scroll and input logic of a context wants presented.
#[repr(C)]
pub struct FfiPresentRequest {
    pub context_id: u64,
    /// When false, the frame is at the viewport rect the context is presented at.
    pub has_viewport_rect: bool,
    pub viewport_rect: IntRect,
    pub forced_damage_rect: IntRect,
    pub scheduling: PresentScheduling,
}

/// Hands over the frames asked for since the last call, in the order they were asked for.
///
/// # Safety
///
/// `receive` must not call back into the core.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_core_take_present_requests(
    core: &CompositorCore,
    receiver: *mut c_void,
    receive: unsafe extern "C" fn(*mut c_void, FfiPresentRequest),
) {
    for request in core.take_present_requests() {
        // SAFETY: The caller's receiver stays out of the core.
        unsafe {
            receive(
                receiver,
                FfiPresentRequest {
                    context_id: request.context_id.0,
                    has_viewport_rect: request.viewport_rect.is_some(),
                    viewport_rect: request.viewport_rect.unwrap_or_default(),
                    forced_damage_rect: request.forced_damage_rect,
                    scheduling: request.scheduling,
                },
            );
        }
    }
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
        now: MonotonicTime::from_nanoseconds(now_nanoseconds),
    };
    // SAFETY: The caller hands over a message of that endpoint.
    unsafe {
        message.hand_to(|bytes, attachments| compositor_web_content_server::dispatch(&mut stub, bytes, attachments));
    };
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
        now: MonotonicTime::from_nanoseconds(now_nanoseconds),
    };
    // SAFETY: The caller hands over a message of that endpoint.
    unsafe {
        message.hand_to(|bytes, attachments| compositor_presentation_server::dispatch(&mut stub, bytes, attachments));
    };
}
