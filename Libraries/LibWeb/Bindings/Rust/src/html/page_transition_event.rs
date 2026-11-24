/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PageTransitionEvent, https://html.spec.whatwg.org/multipage/nav-history-apis.html#pagetransitionevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary PageTransitionEventInit of HTML/PageTransitionEvent.idl.
pub(crate) struct PageTransitionEventInit {
    pub(crate) parent: EventInit,
    pub(crate) persisted: bool,
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#pagetransitionevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PageTransitionEvent {
    event: CppEvent,
    persisted: bool,
}

impl Wrappable for PageTransitionEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl PageTransitionEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: PageTransitionEventInit,
    ) -> Ref<PageTransitionEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| PageTransitionEvent {
            event,
            persisted: event_init.persisted,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: PageTransitionEventInit,
    ) -> WebIdlResult<Ref<PageTransitionEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-pagetransitionevent-persisted
    pub(crate) fn persisted(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.persisted)
    }
}

/// The Event of a new trusted PageTransitionEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_page_transition_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    persisted: bool,
) -> *mut c_void {
    let event_init = PageTransitionEventInit {
        parent: EventInit {
            bubbles,
            cancelable,
            composed,
        },
        persisted,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = PageTransitionEvent::create(vm, realm, &r#type, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}
