/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the scroll and input logic of the contexts asks of the process around it. None of these calls comes back into
//! that logic before it returns.

use crate::overlays::{OverlayLabel, OverlayLabelShaper};
use libcompositing_rust::scrolling::CompositorContextId;
use libcompositing_rust::time::Duration;
use libgfx_rust::IntRect;
use libipc_rust::cpp_connection::RawAttachment;
use libipc_rust::wire::EndpointMessage;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct WebContentClientId(pub u64);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PresentationId(pub u64);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ContextTimer {
    ScrollStepGestureInput,
    ScrollbarPaging,
    ScrollbarFadeOut,
    CaretBlink,
    BackingStoreShrink,
    SurplusStoreRetirement,
    HiddenStoreRetirement,
    SpareTileEviction,
    TileRasterScratchSurfaceRelease,
    RasterScaleOfZoomSettles,
}

/// A timer of the compositor, which fires once after the delay it is set to, until it is set again.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Timer {
    Context(CompositorContextId, ContextTimer),
    /// Releases what the window servers hold off screen for a window resize that its presentation never commits.
    SynchronizedResizeHold,
}

// The host knows a timer by a number of its own, which stays the same for as long as the timer exists.
#[derive(Default)]
struct TimerNumbers {
    next_number: u64,
    number_of_timer: HashMap<Timer, u64>,
    timer_of_number: HashMap<u64, Timer>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PendingFrame {
    pub viewport_rect: IntRect,
}

impl PendingFrame {
    pub fn within(viewport_rect: IntRect) -> Self {
        Self { viewport_rect }
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
    pub scheduling: PresentScheduling,
}

/// What the presentation knows of a context as a tick of a display begins.
#[derive(Clone, Copy, Debug, Default)]
pub struct DisplayTickInputs {
    pub context_is_on_tick_display: bool,
    pub present_is_scheduled_on_tick_display: bool,
    /// Set when the render clock of the context is due a tick now.
    pub clock_tick_interval_in_milliseconds: Option<f64>,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct DisplayTickResult {
    pub presents_on_display: bool,
    /// As of the end of the tick.
    pub needs_animation_frames: bool,
}

/// A label of an overlay as the host shaped it, lent for the call it is handed to.
#[repr(C)]
pub struct FfiOverlayLabel<'a> {
    pub font_id: u64,
    pub glyph_ids: ak::Span<'a, u32>,
    pub glyph_x_positions: ak::Span<'a, f32>,
    pub width: f32,
    pub ascent: f32,
    pub descent: f32,
    pub font_ascent: f32,
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
    /// Arguments: the number of the timer, and the delay in nanoseconds. Setting a timer again replaces its delay, and
    /// a timer that fires calls `compositor_core_timer_fired` with its number.
    pub set_timer: unsafe extern "C" fn(*mut c_void, u64, i64),
    pub cancel_timer: unsafe extern "C" fn(*mut c_void, u64),
    /// The timer is not set again, so the host forgets it.
    pub destroy_timer: unsafe extern "C" fn(*mut c_void, u64),
    /// Arguments: the context, whether a font family is given, the font family, the font size in points, the device
    /// pixel ratio, the text, and what receives the shaped label, which is not called where no font draws the label.
    pub shape_overlay_label: unsafe extern "C" fn(
        *mut c_void,
        u64,
        bool,
        ak::Span<'_, u8>,
        f32,
        f64,
        ak::Span<'_, u8>,
        *mut c_void,
        unsafe extern "C" fn(*mut c_void, *const FfiOverlayLabel<'_>),
    ),
}

type PostEncodedMessage = unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>);

pub struct Host {
    ffi: FfiCompositorCoreHost,
    display_tick_requests: RefCell<Vec<CompositorContextId>>,
    timer_numbers: RefCell<TimerNumbers>,
}

impl Host {
    /// # Safety
    ///
    /// `ffi.host` must outlive the `Host`, and none of its entry points may call back into the core.
    pub unsafe fn new(ffi: FfiCompositorCoreHost) -> Self {
        Self {
            ffi,
            display_tick_requests: RefCell::new(Vec::new()),
            timer_numbers: RefCell::default(),
        }
    }

    fn post<M: EndpointMessage>(&self, post_encoded_message: PostEncodedMessage, receiver_id: u64, message: M) {
        let (bytes, attachments) = message
            .encode_message()
            .expect("a message of the compositor fits an IPC message");
        let raw_attachments: Vec<RawAttachment> = attachments.into_iter().map(RawAttachment::from_attachment).collect();
        // SAFETY: The host outlives this, copies the bytes and takes every attachment over.
        unsafe {
            post_encoded_message(
                self.ffi.host,
                receiver_id,
                ak::Span::new(&bytes),
                ak::Span::new(&raw_attachments),
            );
        }
    }

    /// A message for a client that is gone is dropped.
    pub fn post_to_web_content<M: EndpointMessage>(&self, client_id: WebContentClientId, message: M) {
        self.post(self.ffi.post_message_to_web_content, client_id.0, message);
    }

    /// A client without a render clock channel hears nothing.
    pub fn post_to_render_clock_of_web_content<M: EndpointMessage>(&self, client_id: WebContentClientId, message: M) {
        self.post(
            self.ffi.post_message_to_render_clock_of_web_content,
            client_id.0,
            message,
        );
    }

    /// A message for a presentation that is gone is dropped.
    pub fn post_to_presentation<M: EndpointMessage>(&self, presentation_id: PresentationId, message: M) {
        self.post(self.ffi.post_message_to_presentation, presentation_id.0, message);
    }

    /// Display ticks reach a context whose scroll and input logic says it needs animation frames, while it is visible.
    /// The core asks for them once the call that wants them is done.
    pub fn request_display_ticks(&self, context_id: CompositorContextId) {
        self.display_tick_requests.borrow_mut().push(context_id);
    }

    /// The contexts that asked for display ticks since the last call.
    pub fn take_display_tick_requests(&self) -> Vec<CompositorContextId> {
        std::mem::take(&mut *self.display_tick_requests.borrow_mut())
    }

    /// Each timer of a context fires once after the delay, and setting it again replaces the delay.
    pub fn set_timer(&self, context_id: CompositorContextId, timer: ContextTimer, delay: Duration) {
        self.schedule(Timer::Context(context_id, timer), delay);
    }

    pub fn cancel_timer(&self, context_id: CompositorContextId, timer: ContextTimer) {
        self.unschedule(Timer::Context(context_id, timer));
    }

    pub fn schedule(&self, timer: Timer, delay: Duration) {
        let number = {
            let mut numbers = self.timer_numbers.borrow_mut();
            match numbers.number_of_timer.get(&timer) {
                Some(&number) => number,
                None => {
                    numbers.next_number += 1;
                    let number = numbers.next_number;
                    numbers.number_of_timer.insert(timer, number);
                    numbers.timer_of_number.insert(number, timer);
                    number
                }
            }
        };
        // SAFETY: The host outlives this.
        unsafe { (self.ffi.set_timer)(self.ffi.host, number, delay.nanoseconds()) };
    }

    pub fn unschedule(&self, timer: Timer) {
        let Some(number) = self.timer_numbers.borrow().number_of_timer.get(&timer).copied() else {
            return;
        };
        // SAFETY: The host outlives this.
        unsafe { (self.ffi.cancel_timer)(self.ffi.host, number) };
    }

    /// Forgets the timers that match, which are not set again.
    pub fn destroy_timers(&self, matches: impl Fn(&Timer) -> bool) {
        let numbers: Vec<u64> = {
            let mut timer_numbers = self.timer_numbers.borrow_mut();
            let TimerNumbers {
                number_of_timer,
                timer_of_number,
                ..
            } = &mut *timer_numbers;
            let numbers: Vec<u64> = timer_of_number
                .iter()
                .filter(|(_, timer)| matches(timer))
                .map(|(&number, _)| number)
                .collect();
            for number in &numbers {
                if let Some(timer) = timer_of_number.remove(number) {
                    number_of_timer.remove(&timer);
                }
            }
            numbers
        };
        for number in numbers {
            // SAFETY: The host outlives this.
            unsafe { (self.ffi.destroy_timer)(self.ffi.host, number) };
        }
    }

    pub fn timer_of_number(&self, number: u64) -> Option<Timer> {
        self.timer_numbers.borrow().timer_of_number.get(&number).copied()
    }

    pub fn overlay_label_shaper(&self, context_id: CompositorContextId) -> ContextOverlayLabelShaper<'_> {
        ContextOverlayLabelShaper { host: self, context_id }
    }
}

pub struct ContextOverlayLabelShaper<'a> {
    host: &'a Host,
    context_id: CompositorContextId,
}

impl OverlayLabelShaper for ContextOverlayLabelShaper<'_> {
    fn shape_overlay_label(
        &self,
        font_family: Option<&str>,
        font_size_in_points: f32,
        device_pixel_ratio: f64,
        text: &str,
    ) -> Option<OverlayLabel> {
        unsafe extern "C" fn receive(receiver: *mut c_void, label: *const FfiOverlayLabel<'_>) {
            // SAFETY: The receiver is the label below, and the host lends the shaped label for the call.
            let (receiver, label) = unsafe { (&mut *receiver.cast::<Option<OverlayLabel>>(), &*label) };
            *receiver = Some(OverlayLabel {
                font_id: label.font_id,
                glyph_ids: label.glyph_ids.as_slice().to_vec(),
                glyph_x_positions: label.glyph_x_positions.as_slice().to_vec(),
                width: label.width,
                ascent: label.ascent,
                descent: label.descent,
                font_ascent: label.font_ascent,
            });
        }
        let mut label: Option<OverlayLabel> = None;
        // SAFETY: The host outlives this, shapes synchronously and never calls back into the core.
        unsafe {
            (self.host.ffi.shape_overlay_label)(
                self.host.ffi.host,
                self.context_id.0,
                font_family.is_some(),
                ak::Span::new(font_family.unwrap_or_default().as_bytes()),
                font_size_in_points,
                device_pixel_ratio,
                ak::Span::new(text.as_bytes()),
                (&raw mut label).cast(),
                receive,
            );
        }
        label
    }
}
