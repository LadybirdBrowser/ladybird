/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FontFaceSetLoadEvent, https://drafts.csswg.org/css-font-loading/#fontfacesetloadevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::font_face_set::CppFontFaceState;
use crate::dom::event::EventInit;

/// The dictionary FontFaceSetLoadEventInit of CSS/FontFaceSetLoadEvent.idl.
pub(crate) struct FontFaceSetLoadEventInit<'vm> {
    pub(crate) parent: EventInit,
    pub(crate) fontfaces: MarkedVec<'vm, CppInterface>,
}

// https://drafts.csswg.org/css-font-loading/#fontfacesetloadevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FontFaceSetLoadEvent {
    event: CppEvent,
    fontfaces: Vec<CppFontFaceState>,
}

impl Wrappable for FontFaceSetLoadEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl FontFaceSetLoadEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: EventInit,
        fontfaces: &MarkedVec<'_, CppFontFaceState>,
    ) -> Ref<FontFaceSetLoadEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| FontFaceSetLoadEvent {
            event,
            fontfaces: fontfaces.to_vec(),
        });
        object.event.set_owner(object);
        object
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfacesetloadevent-fontfacesetloadevent
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: FontFaceSetLoadEventInit<'_>,
    ) -> WebIdlResult<Ref<FontFaceSetLoadEvent>> {
        let fontfaces = MarkedVec::with_capacity(cx.vm(), event_init_dict.fontfaces.len());
        for font_face in event_init_dict.fontfaces.values().iter() {
            fontfaces.push(CppFontFaceState::of_wrapper(font_face.wrapper()));
        }
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            event_init_dict.parent,
            &fontfaces,
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfacesetloadevent-fontfaces
    pub(crate) fn fontfaces<'vm>(&self, cx: &mut CallContext<'vm>) -> WebIdlResult<MarkedVec<'vm, CppInterface>> {
        let fontfaces = MarkedVec::with_capacity(cx.vm(), self.fontfaces.len());
        for font_face in &self.fontfaces {
            fontfaces.push(font_face.interface(cx));
        }
        Ok(fontfaces)
    }
}
