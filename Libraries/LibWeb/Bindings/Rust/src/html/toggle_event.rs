/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ToggleEvent, https://html.spec.whatwg.org/multipage/interaction.html#toggleevent

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

/// The dictionary ToggleEventInit of HTML/ToggleEvent.idl.
pub(crate) struct ToggleEventInit {
    pub(crate) parent: EventInit,
    pub(crate) old_state: Utf16String,
    pub(crate) new_state: Utf16String,
    pub(crate) source: Option<CppInterface>,
}

// https://html.spec.whatwg.org/multipage/interaction.html#toggleevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ToggleEvent {
    event: CppEvent,
    old_state: Utf16String,
    new_state: Utf16String,
    source: Option<CppCell>,
}

impl Wrappable for ToggleEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl ToggleEvent {
    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        old_state: Utf16String,
        new_state: Utf16String,
        source: Option<CppCell>,
    ) -> Ref<ToggleEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| ToggleEvent {
            event,
            old_state,
            new_state,
            source,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ToggleEventInit,
    ) -> WebIdlResult<Ref<ToggleEvent>> {
        let source = event_init_dict.source.map(CppInterface::cell);
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            event_init_dict.old_state,
            event_init_dict.new_state,
            source,
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-oldstate
    pub(crate) fn old_state(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.old_state.clone())
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-newstate
    pub(crate) fn new_state(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.new_state.clone())
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-source
    pub(crate) fn source(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // The source getter steps are to return the result of retargeting source against this's currentTarget.
        Ok(self.source.as_ref().map(|source| {
            let source = self.event.retarget_against_current_target(source);
            wrap_cpp_cell(cx, &source)
        }))
    }
}

/// The Event of a new trusted ToggleEvent in the realm, for the implementation to fire.
#[allow(clippy::too_many_arguments, reason = "C++ passes each member of the dictionary")]
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_toggle_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    old_state: LentUtf16FlyString<'_>,
    new_state: LentUtf16FlyString<'_>,
    source: Option<&LentCppCell<cpp_class::Element>>,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let old_state: Utf16String = old_state.to_utf16_fly_string().into();
    let new_state: Utf16String = new_state.to_utf16_fly_string().into();
    let source = source.map(CppCell::from_lent);
    let event = ToggleEvent::create(vm, realm, &r#type, &event_init, old_state, new_state, source);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}
