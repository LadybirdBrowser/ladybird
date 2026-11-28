/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MediaStreamTrackEvent, https://w3c.github.io/mediacapture-main/#mediastreamtrackevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_media_capture_media_stream_track_from_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_media_capture_media_stream_track_wrap(realm: *mut c_void, track: *mut c_void) -> *mut c_void;
}

/// The dictionary MediaStreamTrackEventInit of MediaCapture/MediaStreamTrackEvent.idl.
pub(crate) struct MediaStreamTrackEventInit {
    pub(crate) parent: EventInit,
    pub(crate) track: CppInterface,
}

// https://w3c.github.io/mediacapture-main/#mediastreamtrackevent
#[derive(Trace)]
pub(crate) struct MediaStreamTrackEvent {
    event: CppEvent,
    track: CppCell,
}

impl Wrappable for MediaStreamTrackEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl MediaStreamTrackEvent {
    /// # Safety
    ///
    /// `track` must be a live MediaStreamTrack.
    pub(crate) unsafe fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        track: *mut c_void,
    ) -> Ref<MediaStreamTrackEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        // SAFETY: The caller passes a live MediaStreamTrack.
        let track = unsafe { CppCell::from_raw(track) };
        let object = allocate(vm, |_| MediaStreamTrackEvent { event, track });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: MediaStreamTrackEventInit,
    ) -> WebIdlResult<Ref<MediaStreamTrackEvent>> {
        // SAFETY: The bindings converted the member to a wrapper of a MediaStreamTrack.
        let track = unsafe {
            web_media_capture_media_stream_track_from_wrapper(event_init_dict.track.wrapper().as_ptr().cast())
        };
        // SAFETY: LibWeb returns the live MediaStreamTrack that the wrapper wraps.
        Ok(unsafe { Self::create(cx.vm(), cx.realm(), &r#type, &event_init_dict.parent, track) })
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn track(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // SAFETY: The realm and the MediaStreamTrack are live, and LibWeb returns the wrapper of the MediaStreamTrack.
        let wrapper =
            unsafe { web_media_capture_media_stream_track_wrap(cx.realm().as_ptr().cast(), self.track.as_ptr()) };
        // SAFETY: The wrapper is one of a MediaStreamTrack.
        Ok(unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) })
    }
}

/// The Event of a new trusted MediaStreamTrackEvent in the realm, for the implementation to fire.
///
/// # Safety
///
/// The VM, the realm and the track must be live. The type is lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_media_stream_track_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    track: *mut c_void,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    // SAFETY: The caller passes a live MediaStreamTrack.
    let event = unsafe { MediaStreamTrackEvent::create(vm, realm, &r#type, &event_init, track) };
    event.event().as_ptr()
}
