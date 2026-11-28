/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AnimationPlaybackEvent, https://drafts.csswg.org/web-animations-2/#animationplaybackevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libcompositing_rust::animation_timing::{TimeValue, TimeValueType};
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_numeric_value::{AnyCSSNumericValue, CSSNumberish};
use crate::css::css_unit_value::CSSUnitValue;
use crate::dom::event::EventInit;

/// The dictionary AnimationPlaybackEventInit of Animations/AnimationPlaybackEvent.idl.
pub(crate) struct AnimationPlaybackEventInit {
    pub(crate) parent: EventInit,
    pub(crate) current_time: Option<CSSNumberish>,
    pub(crate) timeline_time: Option<CSSNumberish>,
}

// https://drafts.csswg.org/web-animations-2/#animationplaybackevent
#[derive(Trace)]
pub(crate) struct AnimationPlaybackEvent {
    event: CppEvent,
    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-currenttime
    current_time: Option<CSSNumberish>,
    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-timelinetime
    timeline_time: Option<CSSNumberish>,
}

impl Wrappable for AnimationPlaybackEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

/// A time value as the CSSNumberish that the attributes of the event expose: a double for milliseconds and a
/// CSSUnitValue for a percentage.
pub(crate) fn time_value_as_css_numberish(vm: &Vm, time: TimeValue) -> CSSNumberish {
    match time.r#type {
        TimeValueType::Milliseconds => CSSNumberish::Double(time.value),
        TimeValueType::Percentage => CSSNumberish::CSSNumericValue(AnyCSSNumericValue::CSSUnitValue(
            CSSUnitValue::create(vm, time.value, "percent"),
        )),
    }
}

impl AnimationPlaybackEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: AnimationPlaybackEventInit,
    ) -> Ref<AnimationPlaybackEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| AnimationPlaybackEvent {
            event,
            current_time: event_init.current_time,
            timeline_time: event_init.timeline_time,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: AnimationPlaybackEventInit,
    ) -> WebIdlResult<Ref<AnimationPlaybackEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn current_time(&self, _: &mut CallContext) -> WebIdlResult<Option<CSSNumberish>> {
        Ok(self.current_time)
    }

    pub(crate) fn timeline_time(&self, _: &mut CallContext) -> WebIdlResult<Option<CSSNumberish>> {
        Ok(self.timeline_time)
    }
}
