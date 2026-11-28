/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TextTrackCue, https://html.spec.whatwg.org/multipage/media.html#texttrackcue

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, InterfaceCell, Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::web_vtt::vtt_cue::VTTCue;

unsafe extern "C" {
    fn web_html_text_track_cue_timing_changed(track: *mut c_void);
}

// https://html.spec.whatwg.org/multipage/media.html#texttrackcue
#[derive(Trace)]
pub(crate) struct TextTrackCue {
    event_target: CppEventTarget,

    // The TextTrack whose text track list of cues the cue is in, if any.
    track: GcRefCell<Option<CppCell>>,

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-identifier
    #[gc(untraced)]
    identifier: RefCell<Utf16String>,

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-start-time
    start_time: Cell<f64>,

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-end-time
    end_time: Cell<f64>,

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-pause-on-exit-flag
    pause_on_exit: Cell<bool>,

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-active-flag
    active: Cell<bool>,

    order_added: Cell<u64>,
}

impl Wrappable for TextTrackCue {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for TextTrackCue {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl TextTrackCue {
    pub(crate) fn new(start_time: f64, end_time: f64) -> Self {
        Self {
            event_target: CppEventTarget::new(),
            track: GcRefCell::new(None),
            identifier: RefCell::new(Utf16String::from_utf8("")),
            start_time: Cell::new(start_time),
            end_time: Cell::new(end_time),
            pause_on_exit: Cell::new(false),
            active: Cell::new(false),
            order_added: Cell::new(0),
        }
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn track_cell(&self) -> Option<CppCell> {
        self.track.borrow().clone()
    }

    pub(crate) fn set_track_cell(&self, track: Option<CppCell>) {
        self.track.replace(track);
    }

    pub(crate) fn identifier(&self) -> Utf16String {
        self.identifier.borrow().clone()
    }

    pub(crate) fn set_identifier(&self, identifier: Utf16String) {
        *self.identifier.borrow_mut() = identifier;
    }

    pub(crate) fn start_time_value(&self) -> f64 {
        self.start_time.get()
    }

    pub(crate) fn end_time_value(&self) -> f64 {
        self.end_time.get()
    }

    pub(crate) fn pause_on_exit_flag(&self) -> bool {
        self.pause_on_exit.get()
    }

    pub(crate) fn is_active(&self) -> bool {
        self.active.get()
    }

    pub(crate) fn set_active(&self, active: bool) {
        self.active.set(active);
    }

    pub(crate) fn order_added(&self) -> u64 {
        self.order_added.get()
    }

    pub(crate) fn set_order_added(&self, order_added: u64) {
        self.order_added.set(order_added);
    }

    fn timing_changed(&self) {
        let Some(track) = self.track_cell() else {
            return;
        };
        // SAFETY: The TextTrack is live, which the cue keeps alive.
        unsafe { web_html_text_track_cue_timing_changed(track.as_ptr()) };
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-track
    pub(crate) fn track(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // The track attribute, on getting, must return the TextTrack object of the text track in whose list of cues the
        // text track cue that the TextTrackCue object represents finds itself, if any; or null otherwise.
        Ok(self.track_cell().map(|track| wrap_cpp_cell(cx, &track)))
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The id attribute, on getting, must return the text track cue identifier of the text track cue that the
        // TextTrackCue object represents.
        Ok(self.identifier())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-id
    pub(crate) fn set_id(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        // On setting, the text track cue identifier must be set to the new value.
        self.set_identifier(value);
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-starttime
    pub(crate) fn start_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The startTime attribute, on getting, must return the text track cue start time of the text track cue that the
        // TextTrackCue object represents, in seconds.
        Ok(self.start_time.get())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-starttime
    pub(crate) fn set_start_time(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, the text track cue start time must be set to the new value, interpreted in seconds; then, if the
        // TextTrackCue object's text track cue is in a text track's list of cues, and that text track is in a media
        // element's list of text tracks, and the media element's show poster flag is not set, then run the time marches
        // on steps for that media element.
        self.start_time.set(value);
        self.timing_changed();
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-endtime
    pub(crate) fn end_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The endTime attribute, on getting, must return the text track cue end time of the text track cue that the
        // TextTrackCue object represents, in seconds.
        Ok(self.end_time.get())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-endtime
    pub(crate) fn set_end_time(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // On setting, if the new value is negative Infinity or a Not-a-Number (NaN) value, then throw a TypeError
        // exception.
        if value == f64::NEG_INFINITY || value.is_nan() {
            return Err(Exception::type_error("Value is negative infinity or NaN"));
        }

        // Otherwise, the text track cue end time must be set to the new value.
        self.end_time.set(value);

        // Then, if the TextTrackCue object's text track cue is in a text track's list of cues, and that text track is
        // in a media element's list of text tracks, and the media element's show poster flag is not set, then run the
        // time marches on steps for that media element.
        self.timing_changed();
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-pauseonexit
    pub(crate) fn pause_on_exit(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The pauseOnExit attribute, on getting, must return true if the text track cue pause-on-exit flag of the text
        // track cue that the TextTrackCue object represents is set; or false otherwise.
        Ok(self.pause_on_exit.get())
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-pauseonexit
    pub(crate) fn set_pause_on_exit(&self, _: &mut CallContext, value: bool) -> WebIdlResult<()> {
        // On setting, the text track cue pause-on-exit flag must be set if the new value is true, and must be unset
        // otherwise.
        self.pause_on_exit.set(value);
        Ok(())
    }
}

/// An object of TextTrackCue or of an interface that inherits from it, as LibWeb's C++ code names it.
pub(crate) enum AnyTextTrackCue {
    TextTrackCue(Ref<TextTrackCue>),
    VTTCue(Ref<VTTCue>),
}

impl Deref for AnyTextTrackCue {
    type Target = TextTrackCue;

    fn deref(&self) -> &TextTrackCue {
        match self {
            Self::TextTrackCue(cue) => cue,
            Self::VTTCue(cue) => {
                let cue: &VTTCue = cue;
                cue.parent()
            }
        }
    }
}

impl AnyTextTrackCue {
    pub(crate) fn from_abi(cell: &InterfaceCell) -> Self {
        if let Some(cue) = cell.object_if::<VTTCue>() {
            return Self::VTTCue(cue);
        }
        Self::TextTrackCue(cell.object::<TextTrackCue>())
    }
}
