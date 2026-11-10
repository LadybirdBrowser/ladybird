/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ProgressEvent, https://xhr.spec.whatwg.org/#interface-progressevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

// https://xhr.spec.whatwg.org/#dictdef-progresseventinit
pub(crate) struct ProgressEventInit {
    pub(crate) parent: EventInit,
    pub(crate) length_computable: bool,
    pub(crate) loaded: f64,
    pub(crate) total: f64,
}

// https://xhr.spec.whatwg.org/#interface-progressevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ProgressEvent {
    event: CppEvent,
    length_computable: bool,
    loaded: f64,
    total: f64,
}

impl Wrappable for ProgressEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

// https://xhr.spec.whatwg.org/#interface-progressevent
impl ProgressEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: ProgressEventInit,
    ) -> Ref<ProgressEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| ProgressEvent {
            event,
            length_computable: event_init.length_computable,
            loaded: event_init.loaded,
            total: event_init.total,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ProgressEventInit,
    ) -> WebIdlResult<Ref<ProgressEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn length_computable(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.length_computable)
    }

    pub(crate) fn loaded(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.loaded)
    }

    pub(crate) fn total(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.total)
    }
}
