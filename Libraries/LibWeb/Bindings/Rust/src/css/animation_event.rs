/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AnimationEvent, https://drafts.csswg.org/css-animations-2/#interface-animationevent

use core::cell::RefCell;
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
/// NB: The event lets go of the record as it is finalized rather than as it is swept, after the elements the record
///     names may be gone, so that the record never outlives them alive.
pub(crate) struct EventAnimation(RefCell<Option<AnimationRecordRef>>);

impl EventAnimation {
    /// The animation of a record of it.
    pub(crate) fn of_record(animation: AnimationRecordRef) -> Self {
        Self(RefCell::new(Some(animation)))
    }

    /// The animation of an object of it.
    pub(crate) fn of_object(animation: &Animation) -> Self {
        Self::of_record(animation.record().clone())
    }

    /// The object of the animation, which a CSS animation or transition gets as script first asks for it.
    pub(crate) fn wrap(&self, cx: &CallContext) -> AnyAnimation {
        let animation = self.0.borrow();
        animation_object(
            cx.vm(),
            animation.as_ref().expect("an event that is alive holds its animation"),
        )
    }

    /// Lets go of the record, as the event is finalized.
    pub(crate) fn release(&self) {
        self.0.borrow_mut().take();
    }
}

// SAFETY: Visits the cells of the host that keep the record's object, or the elements the record names, alive.
unsafe impl Trace for EventAnimation {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        if let Some(animation) = self.0.borrow().as_ref() {
            animation.for_each_host_cell(|cell, _| {
                // SAFETY: The record names live cells of the host, which the event keeps alive.
                visitor.visit(unsafe { Gc::from_non_null(cell) });
            });
        }
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
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        if let Some(animation) = &object.animation {
            animation.release();
        }
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
