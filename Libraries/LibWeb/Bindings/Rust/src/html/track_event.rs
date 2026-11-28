/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TrackEvent, https://html.spec.whatwg.org/multipage/media.html#trackevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::dom::event::EventInit;

/// The dictionary TrackEventInit of HTML/TrackEvent.idl.
pub(crate) struct TrackEventInit {
    pub(crate) parent: EventInit,
    pub(crate) track: Option<CppInterface>,
}

// https://html.spec.whatwg.org/multipage/media.html#trackevent
#[derive(Trace)]
pub(crate) struct TrackEvent {
    event: CppEvent,
    track: Option<CppCell>,
}

impl Wrappable for TrackEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl TrackEvent {
    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        track: Option<CppCell>,
    ) -> Ref<TrackEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| TrackEvent { event, track });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: TrackEventInit,
    ) -> WebIdlResult<Ref<TrackEvent>> {
        let track = event_init_dict.track.map(CppInterface::cell);
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            track,
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-trackevent-track
    pub(crate) fn track(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.track.as_ref().map(|track| wrap_cpp_cell(cx, track)))
    }
}

/// The Event of a new trusted TrackEvent in the realm, for the implementation to fire. The track is a VideoTrack, an
/// AudioTrack or a TextTrack.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_track_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    track: &LentCppCell<cpp_class::Cell>,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles: false,
        cancelable: false,
        composed: false,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let track = CppCell::from_lent(track);
    let event = TrackEvent::create(vm, realm, &r#type, &event_init, Some(track));
    event.event().as_ptr()
}
