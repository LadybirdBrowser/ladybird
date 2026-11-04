/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StorageEvent, https://html.spec.whatwg.org/multipage/webstorage.html#storageevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Realm, Trace, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;
use crate::html::storage::Storage;

/// The dictionary StorageEventInit of HTML/StorageEvent.idl.
pub(crate) struct StorageEventInit {
    pub(crate) parent: EventInit,
    pub(crate) key: Option<Utf16String>,
    pub(crate) old_value: Option<Utf16String>,
    pub(crate) new_value: Option<Utf16String>,
    pub(crate) url: UsvString,
    pub(crate) storage_area: Option<Ref<Storage>>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct StorageEventState {
    key: Option<Utf16String>,
    old_value: Option<Utf16String>,
    new_value: Option<Utf16String>,
    #[gc(untraced)]
    url: UsvString,
    storage_area: Option<Ref<Storage>>,
}

// https://html.spec.whatwg.org/multipage/webstorage.html#storageevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StorageEvent {
    event: CppEvent,
    state: GcRefCell<StorageEventState>,
}

impl Wrappable for StorageEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl StorageEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: StorageEventInit,
    ) -> Ref<StorageEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| StorageEvent {
            event,
            state: GcRefCell::new(StorageEventState {
                key: event_init.key,
                old_value: event_init.old_value,
                new_value: event_init.new_value,
                url: event_init.url,
                storage_area: event_init.storage_area,
            }),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: StorageEventInit,
    ) -> WebIdlResult<Ref<StorageEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn key(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.state.borrow().key.clone())
    }

    pub(crate) fn old_value(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.state.borrow().old_value.clone())
    }

    pub(crate) fn new_value(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.state.borrow().new_value.clone())
    }

    pub(crate) fn url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.state.borrow().url.clone())
    }

    pub(crate) fn storage_area(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<Storage>>> {
        Ok(self.state.borrow().storage_area)
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storageevent-initstorageevent
    #[allow(clippy::too_many_arguments, reason = "the operation has these arguments")]
    pub(crate) fn init_storage_event(
        &self,
        cx: &mut CallContext,
        r#type: Utf16String,
        bubbles: bool,
        cancelable: bool,
        key: Option<Utf16String>,
        old_value: Option<Utf16String>,
        new_value: Option<Utf16String>,
        url: UsvString,
        storage_area: Option<Ref<Storage>>,
    ) -> WebIdlResult<()> {
        // The initStorageEvent(type, bubbles, cancelable, key, oldValue, newValue, url, storageArea) method must initialize
        // the event in a manner analogous to the similarly-named initEvent() method. [DOM]
        if self.event.dispatched() {
            return Ok(());
        }

        self.event.initialize_event(&r#type, bubbles, cancelable);
        *self.state.borrow_mut(cx.vm()) = StorageEventState {
            key,
            old_value,
            new_value,
            url,
            storage_area,
        };
        Ok(())
    }
}
