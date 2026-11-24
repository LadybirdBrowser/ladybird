/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FormDataEvent, https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#the-formdataevent-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;
use crate::xhr::form_data::FormData;

/// The dictionary FormDataEventInit of HTML/FormDataEvent.idl.
pub(crate) struct FormDataEventInit {
    pub(crate) parent: EventInit,
    pub(crate) form_data: Ref<FormData>,
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#formdataevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FormDataEvent {
    event: CppEvent,
    form_data: Ref<FormData>,
}

impl Wrappable for FormDataEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl FormDataEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: FormDataEventInit,
    ) -> Ref<FormDataEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| FormDataEvent {
            event,
            form_data: event_init.form_data,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: FormDataEventInit,
    ) -> WebIdlResult<Ref<FormDataEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-formdataevent-formdata
    pub(crate) fn form_data(&self, _: &mut CallContext) -> WebIdlResult<Ref<FormData>> {
        Ok(self.form_data)
    }
}

/// The Event of a new trusted FormDataEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_form_data_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    form_data: &InterfaceCell,
) -> *mut c_void {
    let event_init = FormDataEventInit {
        parent: EventInit {
            bubbles,
            cancelable,
            composed,
        },
        form_data: form_data.object::<FormData>(),
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = FormDataEvent::create(vm, realm, &r#type, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}
