/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SubmitEvent, https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#submitevent

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

/// The dictionary SubmitEventInit of HTML/SubmitEvent.idl.
pub(crate) struct SubmitEventInit {
    pub(crate) parent: EventInit,
    pub(crate) submitter: Option<CppInterface>,
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#submitevent
#[derive(Trace)]
pub(crate) struct SubmitEvent {
    event: CppEvent,
    submitter: Option<CppCell>,
}

impl Wrappable for SubmitEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl SubmitEvent {
    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        submitter: Option<CppCell>,
    ) -> Ref<SubmitEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| SubmitEvent { event, submitter });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: SubmitEventInit,
    ) -> WebIdlResult<Ref<SubmitEvent>> {
        let submitter = event_init_dict.submitter.map(CppInterface::cell);
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            submitter,
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-submitevent-submitter
    pub(crate) fn submitter(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.submitter.as_ref().map(|submitter| wrap_cpp_cell(cx, submitter)))
    }
}

/// The Event of a new trusted SubmitEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_submit_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    submitter: Option<&LentCppCell<cpp_class::Element>>,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let submitter = submitter.map(CppCell::from_lent);
    let event = SubmitEvent::create(vm, realm, &r#type, &event_init, submitter);
    event.event().as_ptr()
}
