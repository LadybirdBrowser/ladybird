/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TransitionEvent, https://drafts.csswg.org/css-transitions-2/#interface-transitionevent-idl

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::animation_event::EventAnimation;
use super::css_transition::CSSTransition;
use crate::animations::animation::AnyAnimation;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::Inherits;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary TransitionEventInit of CSS/TransitionEvent.idl.
pub(crate) struct TransitionEventInit {
    pub(crate) parent: EventInit,
    pub(crate) animation: Option<Ref<CSSTransition>>,
    pub(crate) elapsed_time: f64,
    pub(crate) property_name: Utf16String,
    pub(crate) pseudo_element: Utf16String,
}

// https://drafts.csswg.org/css-transitions-2/#interface-transitionevent-idl
#[derive(Trace)]
pub(crate) struct TransitionEvent {
    event: CppEvent,
    #[gc(untraced)]
    property_name: Utf16String,
    elapsed_time: f64,
    #[gc(untraced)]
    pseudo_element: Utf16String,
    animation: Option<EventAnimation>,
}

impl Wrappable for TransitionEvent {
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

impl TransitionEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: TransitionEventInit,
    ) -> Ref<TransitionEvent> {
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
        event_init: TransitionEventInit,
        animation: Option<EventAnimation>,
    ) -> Ref<TransitionEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| TransitionEvent {
            event,
            property_name: event_init.property_name,
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
        transition_event_init_dict: TransitionEventInit,
    ) -> WebIdlResult<Ref<TransitionEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, transition_event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn property_name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.property_name.clone())
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
