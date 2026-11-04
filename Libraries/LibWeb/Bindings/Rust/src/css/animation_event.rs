/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AnimationEvent, https://drafts.csswg.org/css-animations-2/#interface-animationevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::css_rule::wrap_cpp_cell;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_css_animation_of_wrapper(wrapper: *mut c_void) -> *mut c_void;
}

/// The Animation behind a wrapper of it, which an event holds to wrap it again for the world of each caller.
pub(crate) fn animation_of_wrapper(animation: CppInterface) -> CppCell {
    // SAFETY: The caller passes a wrapper of an Animation, whose cell is live.
    unsafe { CppCell::from_raw(web_css_animation_of_wrapper(animation.wrapper().as_ptr().cast())) }
}

/// The dictionary AnimationEventInit of CSS/AnimationEvent.idl.
pub(crate) struct AnimationEventInit {
    pub(crate) parent: EventInit,
    pub(crate) animation: Option<CppInterface>,
    pub(crate) animation_name: Utf16String,
    pub(crate) elapsed_time: f64,
    pub(crate) pseudo_element: Utf16String,
}

// https://drafts.csswg.org/css-animations-2/#interface-animationevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AnimationEvent {
    event: CppEvent,
    #[gc(untraced)]
    animation_name: Utf16String,
    elapsed_time: f64,
    #[gc(untraced)]
    pseudo_element: Utf16String,
    animation: Option<CppCell>,
}

impl Wrappable for AnimationEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl AnimationEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: AnimationEventInit,
    ) -> Ref<AnimationEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| AnimationEvent {
            event,
            animation_name: event_init.animation_name,
            elapsed_time: event_init.elapsed_time,
            pseudo_element: event_init.pseudo_element,
            animation: event_init.animation.map(animation_of_wrapper),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        animation_event_init_dict: AnimationEventInit,
    ) -> WebIdlResult<Ref<AnimationEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, animation_event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn animation_name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.animation_name.clone())
    }

    pub(crate) fn elapsed_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.elapsed_time)
    }

    pub(crate) fn pseudo_element(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.pseudo_element.clone())
    }

    pub(crate) fn animation(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.animation.as_ref().map(|animation| wrap_cpp_cell(cx, animation)))
    }
}
