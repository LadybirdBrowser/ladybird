/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PictureInPictureEvent, https://w3c.github.io/picture-in-picture/#pictureinpictureevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_picture_in_picture_window_from_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_picture_in_picture_window_wrap(realm: *mut c_void, window: *mut c_void) -> *mut c_void;
}

/// The dictionary PictureInPictureEventInit of PictureInPicture/PictureInPictureEvent.idl.
pub(crate) struct PictureInPictureEventInit {
    pub(crate) parent: EventInit,
    pub(crate) picture_in_picture_window: CppInterface,
}

// https://w3c.github.io/picture-in-picture/#pictureinpictureevent
#[derive(Trace)]
pub(crate) struct PictureInPictureEvent {
    event: CppEvent,
    picture_in_picture_window: CppCell,
}

impl Wrappable for PictureInPictureEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl PictureInPictureEvent {
    /// # Safety
    ///
    /// `window` must be a live PictureInPictureWindow.
    pub(crate) unsafe fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        window: *mut c_void,
    ) -> Ref<PictureInPictureEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        // SAFETY: The caller passes a live PictureInPictureWindow.
        let picture_in_picture_window = unsafe { CppCell::from_raw(window) };
        let object = allocate(vm, |_| PictureInPictureEvent {
            event,
            picture_in_picture_window,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: PictureInPictureEventInit,
    ) -> WebIdlResult<Ref<PictureInPictureEvent>> {
        // SAFETY: The bindings converted the member to a wrapper of a PictureInPictureWindow.
        let window = unsafe {
            web_picture_in_picture_window_from_wrapper(
                event_init_dict.picture_in_picture_window.wrapper().as_ptr().cast(),
            )
        };
        // SAFETY: LibWeb returns the live PictureInPictureWindow that the wrapper wraps.
        Ok(unsafe { Self::create(cx.vm(), cx.realm(), &r#type, &event_init_dict.parent, window) })
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn picture_in_picture_window(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // SAFETY: The realm and the window are live, and LibWeb returns the wrapper of the window.
        let wrapper = unsafe {
            web_picture_in_picture_window_wrap(cx.realm().as_ptr().cast(), self.picture_in_picture_window.as_ptr())
        };
        // SAFETY: The wrapper is one of a PictureInPictureWindow.
        Ok(unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) })
    }
}

/// The Event of a new trusted PictureInPictureEvent in the realm, for the implementation to fire.
///
/// # Safety
///
/// The VM, the realm and the window must be live. The type is lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_picture_in_picture_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    window: *mut c_void,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    // SAFETY: The caller passes a live PictureInPictureWindow.
    let event = unsafe { PictureInPictureEvent::create(vm, realm, &r#type, &event_init, window) };
    event.event().as_ptr()
}
