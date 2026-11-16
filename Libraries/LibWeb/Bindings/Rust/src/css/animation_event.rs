/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AnimationEvent, https://drafts.csswg.org/css-animations-2/#interface-animationevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Visitor, Vm};
use libweb_rust::AnimationRecordRef;

use super::css_animation::CSSAnimation;
use crate::animations::animation::{Animation, AnyAnimation, animation_object};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::Inherits;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The animation an event names, as its record. The event visits what keeps the record's object, or the elements the
/// record names, alive.
pub(crate) struct EventAnimation(AnimationRecordRef);

unsafe extern "C" {
    fn web_animation_visit_host_cells(animation: *const c_void, visitor: *mut c_void);
}

impl EventAnimation {
    /// # Safety
    ///
    /// `animation` must be an animation the host holds a reference to.
    pub(crate) unsafe fn retained_from_host(animation: *const c_void) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self(unsafe { AnimationRecordRef::retained_from_host(animation) })
    }

    /// The animation of an object of it.
    pub(crate) fn of_object(animation: &Animation) -> Self {
        Self(animation.record().clone())
    }

    /// The object of the animation, which a CSS animation or transition gets as script first asks for it.
    pub(crate) fn wrap(&self, cx: &CallContext) -> AnyAnimation {
        animation_object(cx.vm(), &self.0)
    }
}

// SAFETY: Visits the cells of the host that keep the record's object, or the elements the record names, alive.
unsafe impl Trace for EventAnimation {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: The event holds a reference to the record, and the visitor is one LibGC passed in.
        unsafe { web_animation_visit_host_cells(self.0.as_host_ptr(), visitor.as_raw().cast()) };
    }
}

/// The dictionary AnimationEventInit of CSS/AnimationEvent.idl.
pub(crate) struct AnimationEventInit {
    pub(crate) parent: EventInit,
    pub(crate) animation: Option<Ref<CSSAnimation>>,
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
    animation: Option<EventAnimation>,
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
        let animation = event_init
            .animation
            .as_ref()
            .map(|animation| EventAnimation::of_object(animation.parent()));
        Self::create_for_animation(vm, realm, event_name, event_init, animation)
    }

    /// A new event that names `animation` in place of the animation of `event_init`.
    pub(crate) fn create_for_animation(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: AnimationEventInit,
        animation: Option<EventAnimation>,
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
            animation,
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

    pub(crate) fn animation(&self, cx: &mut CallContext) -> WebIdlResult<Option<AnyAnimation>> {
        Ok(self.animation.as_ref().map(|animation| animation.wrap(cx)))
    }
}
