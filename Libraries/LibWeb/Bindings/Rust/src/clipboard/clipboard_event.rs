/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ClipboardEvent, https://w3c.github.io/clipboard-apis/#clipboardevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_clipboard_data_transfer_from_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_clipboard_data_transfer_wrap(realm: *mut c_void, data_transfer: *mut c_void) -> *mut c_void;
}

/// The dictionary ClipboardEventInit of Clipboard/ClipboardEvent.idl.
pub(crate) struct ClipboardEventInit {
    pub(crate) parent: EventInit,
    pub(crate) clipboard_data: Option<CppInterface>,
}

// https://w3c.github.io/clipboard-apis/#clipboardevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ClipboardEvent {
    event: CppEvent,
    clipboard_data: Option<CppCell>,
}

impl Wrappable for ClipboardEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl ClipboardEvent {
    /// # Safety
    ///
    /// `clipboard_data` must be null or a live DataTransfer.
    pub(crate) unsafe fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        clipboard_data: *mut c_void,
    ) -> Ref<ClipboardEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        // SAFETY: The caller passes null or a live DataTransfer.
        let clipboard_data = NonNull::new(clipboard_data).map(|cell| unsafe { CppCell::from_raw(cell.as_ptr()) });
        let object = allocate(vm, |_| ClipboardEvent { event, clipboard_data });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ClipboardEventInit,
    ) -> WebIdlResult<Ref<ClipboardEvent>> {
        let clipboard_data = event_init_dict
            .clipboard_data
            .map_or(core::ptr::null_mut(), |clipboard_data| {
                // SAFETY: The bindings converted the member to a wrapper of a DataTransfer.
                unsafe { web_clipboard_data_transfer_from_wrapper(clipboard_data.wrapper().as_ptr().cast()) }
            });
        // SAFETY: LibWeb returns the live DataTransfer that the wrapper wraps.
        Ok(unsafe { Self::create(cx.vm(), cx.realm(), &r#type, &event_init_dict.parent, clipboard_data) })
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn clipboard_data(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.clipboard_data.as_ref().map(|clipboard_data| {
            // SAFETY: The realm and the DataTransfer are live, and LibWeb returns the wrapper of the DataTransfer.
            let wrapper =
                unsafe { web_clipboard_data_transfer_wrap(cx.realm().as_ptr().cast(), clipboard_data.as_ptr()) };
            // SAFETY: The wrapper is one of a DataTransfer.
            unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) }
        }))
    }
}
