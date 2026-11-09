/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points of the scroll and input logic for the C++ of the Compositor around it.

use crate::context_state::ScrollCapabilities;
use crate::core::messages::{MessagesFromPresentation, MessagesFromWebContent};
use crate::core::{CompositorCore, ContextOwnerCheckResult, VisualContextTreeUpdateResult};
use crate::frame_inputs::{FrameInputs, ScrollbarOfFrame};
use crate::host::{
    ContextTimer, DisplayTickInputs, DisplayTickResult, FfiCompositorCoreHost, Host, PresentScheduling, PresentationId,
    WebContentClientId,
};
use libcompositing_rust::display_list::commands::{
    CanvasId, CompositorContextId, ImageFrameResourceId, VideoSinkResourceId,
};
use libcompositing_rust::display_list::damage::LiveContentChanges;
use libcompositing_rust::display_list::storage::storage_from_handle;
use libcompositing_rust::ffi::{
    FfiDisplayList, replay_display_list_storage, replay_nested_records, tree_sharing_handle,
};
use libcompositing_rust::host::FfiDisplayListReplayCallbacks;
use libcompositing_rust::ipc_types::{
    Compositing_ContextVisibility as ContextVisibility, compositor_presentation_server, compositor_web_content_server,
};
use libcompositing_rust::scrolling::wheel_gesture_identity::TouchpadGestureEnd;
use libcompositing_rust::scrolling::{KeyboardScrollState, ScrollStateSnapshot};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{Color, IntRect, IntSize};
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

/// What a frame of the context is painted with besides the display list and its resources, until
/// `compositor_frame_inputs_destroy` frees it. Each frame shows the scrollbars as of when it is painted, so this updates
/// which of them are shown. Null for a context that has no display list yet.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_frame_inputs(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
) -> *mut FrameInputs {
    core.frame_inputs(
        CompositorContextId(context_id),
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    )
    .map_or(std::ptr::null_mut(), |frame_inputs| {
        Box::into_raw(Box::new(frame_inputs))
    })
}

/// What a screenshot of the context is painted with, until `compositor_frame_inputs_destroy` frees it, with the
/// scrollbar fade damage that the screenshot does not present on screen. The context must have a display list.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_core_frame_inputs_for_screenshot(
    core: &CompositorCore,
    context_id: u64,
    now_nanoseconds: i64,
    scrollbar_fade_damage_rect: &mut IntRect,
) -> *mut FrameInputs {
    let screenshot_inputs = core.frame_inputs_for_screenshot(
        CompositorContextId(context_id),
        MonotonicTime::from_nanoseconds(now_nanoseconds),
    );
    *scrollbar_fade_damage_rect = screenshot_inputs.scrollbar_fade_damage_rect;
    Box::into_raw(Box::new(screenshot_inputs.frame_inputs))
}

/// # Safety
///
/// `frame_inputs` must be inputs the core handed out that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_frame_inputs_destroy(frame_inputs: *mut FrameInputs) {
    // SAFETY: The caller hands over the inputs the core made.
    drop(unsafe { Box::from_raw(frame_inputs) });
}

/// One strong reference to the visual context tree the frame is painted with, which the caller takes over.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_frame_inputs_retain_visual_context_tree(frame_inputs: &FrameInputs) -> *const c_void {
    Arc::into_raw(frame_inputs.visual_context_tree.as_arc().clone()).cast()
}

/// The scrollbars the frame shows, of which there are `count`, lent for as long as the inputs live.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_frame_inputs_scrollbars(
    frame_inputs: &FrameInputs,
    count: &mut usize,
) -> *const ScrollbarOfFrame {
    *count = frame_inputs.scrollbars.len();
    frame_inputs.scrollbars.as_ptr()
}

/// Replays the list `display_list_storage` holds as the frame shows it, scrolled where the frame presents it.
///
/// # Safety
///
/// `display_list_storage` must be a live storage handle of the list the context holds, and `callbacks` must be live.
/// The painter callbacks run synchronously and may re-enter a replay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_frame_inputs_replay(
    frame_inputs: &FrameInputs,
    display_list_storage: *const c_void,
    root_group_color: Color,
    callbacks: *const FfiDisplayListReplayCallbacks,
) {
    // SAFETY: The caller lends a live storage handle and live callbacks.
    unsafe {
        replay_display_list_storage(
            frame_inputs.visual_context_tree.as_arc(),
            display_list_storage,
            root_group_color,
            frame_inputs.presented_scroll_state_snapshot.device_offsets(),
            callbacks,
        );
    }
}

/// Plays a stream of records that a command of the list being replayed nests, scrolled where the frame presents it.
///
/// # Safety
///
/// `records` must address `records_size` bytes of a tape that is valid for the call, and `callbacks` must be live.
/// The painter callbacks run synchronously and may re-enter a replay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_frame_inputs_replay_records(
    frame_inputs: &FrameInputs,
    records: *const u8,
    records_size: usize,
    callbacks: *const FfiDisplayListReplayCallbacks,
) {
    // SAFETY: The caller lends the stated number of bytes and live callbacks.
    unsafe {
        replay_nested_records(
            ak::slice_from_raw_parts(records, records_size),
            frame_inputs.presented_scroll_state_snapshot.device_offsets(),
            callbacks,
        );
    }
}

/// What a frame painted with `frame_inputs` repaints of the frame painted last, with `last_frame_inputs` and the list
/// `last_display_list_storage` holds. Writes the damage rect through `damage_rect` and returns whether the damage is
/// bounded; unbounded damage means the whole viewport must repaint.
///
/// # Safety
///
/// Both storages must be live storage handles, and the id pointers must address the stated number of ids for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_frame_inputs_damage_since(
    frame_inputs: &FrameInputs,
    display_list_storage: *const c_void,
    last_frame_inputs: &FrameInputs,
    last_display_list_storage: *const c_void,
    viewport_rect: IntRect,
    canvases_with_new_content: *const CanvasId,
    canvases_with_new_content_len: usize,
    video_sinks_with_new_frames: *const VideoSinkResourceId,
    video_sinks_with_new_frames_len: usize,
    image_frames_with_new_pixels: *const ImageFrameResourceId,
    image_frames_with_new_pixels_len: usize,
    forced_damage_rect: IntRect,
    damage_rect: &mut IntRect,
) -> bool {
    // SAFETY: The caller guarantees live handles and valid slices for the duration of the call.
    let (display_list, last_display_list, live_content_changes) = unsafe {
        (
            storage_from_handle(display_list_storage),
            storage_from_handle(last_display_list_storage),
            LiveContentChanges {
                canvases: ak::slice_from_raw_parts(canvases_with_new_content, canvases_with_new_content_len),
                video_sinks: ak::slice_from_raw_parts(video_sinks_with_new_frames, video_sinks_with_new_frames_len),
                image_frames: ak::slice_from_raw_parts(image_frames_with_new_pixels, image_frames_with_new_pixels_len),
                forced_damage_rect,
            },
        )
    };
    let damage = frame_inputs.damage_since(
        display_list,
        last_frame_inputs,
        last_display_list,
        viewport_rect,
        live_content_changes,
    );
    match damage {
        Some(bounded_damage_rect) => {
            *damage_rect = bounded_damage_rect;
            true
        }
        None => false,
    }
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
