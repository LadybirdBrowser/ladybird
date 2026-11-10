/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the scroll and input logic of the contexts asks of the process around it. None of these calls comes back into
//! that logic before it returns.

use libcompositing_rust::scrolling::CompositorContextId;
use libcompositing_rust::time::Duration;
use libgfx_rust::IntRect;
use libipc_rust::cpp_connection::RawAttachment;
use libipc_rust::wire::EndpointMessage;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct WebContentClientId(pub u64);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PresentationId(pub u64);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ContextTimer {
    ScrollStepGestureInput,
    ScrollbarPaging,
    ScrollbarFadeOut,
    CaretBlink,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PendingFrame {
    pub viewport_rect: IntRect,
    pub forced_damage_rect: IntRect,
}

impl PendingFrame {
    pub fn repainting_everything(viewport_rect: IntRect) -> Self {
        Self {
            viewport_rect,
            forced_damage_rect: IntRect::new(0, 0, viewport_rect.width, viewport_rect.height),
        }
    }

    pub fn repainting_changes(viewport_rect: IntRect) -> Self {
        Self {
            viewport_rect,
            forced_damage_rect: IntRect::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PresentScheduling {
    /// The frame is presented on the next tick of the context's display, which the request asks for.
    OnNextDisplayTick,
    /// The frame joins what the display tick in progress presents.
    WithDisplayTickInProgress,
}

/// A frame that the scroll and input logic of a context wants presented. The compositor schedules it once the call
/// that asked for it has returned.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PresentRequest {
    pub context_id: CompositorContextId,
    /// Empty for a frame at the viewport rect the context is presented at.
    pub viewport_rect: Option<IntRect>,
    pub forced_damage_rect: IntRect,
    pub scheduling: PresentScheduling,
}

/// What the presentation knows of a context as a tick of a display begins.
#[derive(Clone, Copy, Debug, Default)]
pub struct DisplayTickInputs {
    pub context_is_on_tick_display: bool,
    pub present_is_scheduled_on_tick_display: bool,
    pub ui_overlay_fades_need_frame: bool,
    /// Set when the render clock of the context is due a tick now.
    pub clock_tick_interval_in_milliseconds: Option<f64>,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct DisplayTickResult {
    pub presents_on_display: bool,
    /// As of the end of the tick, and without the fades of the UI overlays.
    pub needs_animation_frames: bool,
}

/// The entry points of the host and the object they are called on: `Compositor::RustFFI::FfiCompositorCoreHost`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCompositorCoreHost {
    pub host: *mut c_void,
    pub post_message_to_web_content:
        unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>),
    pub post_message_to_render_clock_of_web_content:
        unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>),
    pub post_message_to_presentation:
        unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>),
    pub request_display_ticks: unsafe extern "C" fn(*mut c_void, u64),
    pub set_timer: unsafe extern "C" fn(*mut c_void, u64, ContextTimer, i64),
    pub cancel_timer: unsafe extern "C" fn(*mut c_void, u64, ContextTimer),
}

type PostEncodedMessage = unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>);

pub struct Host(FfiCompositorCoreHost);

impl Host {
    /// # Safety
    ///
    /// `ffi.host` must outlive the `Host`, and none of its entry points may call back into the core.
    pub unsafe fn new(ffi: FfiCompositorCoreHost) -> Self {
        Self(ffi)
    }

    fn post<M: EndpointMessage>(&self, post_encoded_message: PostEncodedMessage, receiver_id: u64, message: M) {
        let (bytes, attachments) = message
            .encode_message()
            .expect("a message of the compositor fits an IPC message");
        let raw_attachments: Vec<RawAttachment> = attachments.into_iter().map(RawAttachment::from_attachment).collect();
        // SAFETY: The host outlives this, copies the bytes and takes every attachment over.
        unsafe {
            post_encoded_message(
                self.0.host,
                receiver_id,
                ak::Span::new(&bytes),
                ak::Span::new(&raw_attachments),
            );
        }
    }

    /// A message for a client that is gone is dropped.
    pub fn post_to_web_content<M: EndpointMessage>(&self, client_id: WebContentClientId, message: M) {
        self.post(self.0.post_message_to_web_content, client_id.0, message);
    }

    /// A client without a render clock channel hears nothing.
    pub fn post_to_render_clock_of_web_content<M: EndpointMessage>(&self, client_id: WebContentClientId, message: M) {
        self.post(self.0.post_message_to_render_clock_of_web_content, client_id.0, message);
    }

    /// A message for a presentation that is gone is dropped.
    pub fn post_to_presentation<M: EndpointMessage>(&self, presentation_id: PresentationId, message: M) {
        self.post(self.0.post_message_to_presentation, presentation_id.0, message);
    }

    /// Display ticks reach a context whose scroll and input logic says it needs animation frames, while it is visible.
    pub fn request_display_ticks(&self, context_id: CompositorContextId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.request_display_ticks)(self.0.host, context_id.0) };
    }

    /// Each timer of a context fires once after the delay, and setting it again replaces the delay.
    pub fn set_timer(&self, context_id: CompositorContextId, timer: ContextTimer, delay: Duration) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.set_timer)(self.0.host, context_id.0, timer, delay.nanoseconds()) };
    }

    pub fn cancel_timer(&self, context_id: CompositorContextId, timer: ContextTimer) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.cancel_timer)(self.0.host, context_id.0, timer) };
    }
}
