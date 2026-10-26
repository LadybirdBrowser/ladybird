/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The EventTarget or Event that an object of an interface implemented in Rust owns when its interface inherits from
//! one of them, which LibWeb implements in C++ (Libraries/LibWeb/Bindings/RustEventTarget.h). The object's wrappers
//! hold it as their wrappable, so that the C++ bindings of EventTarget and Event run on it, and its wrappers are the
//! object's.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Value};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Interface, Ref, Wrappable, cell_from_abi, cell_into_abi};

unsafe extern "C" {
    fn web_bindings_create_event_target() -> *mut c_void;
    fn web_bindings_set_event_target_owner(
        event_target: *mut c_void,
        interface_name: u16,
        owner: *mut c_void,
        get_the_parent: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    );
    fn web_bindings_event_handler_attribute(event_target: *mut c_void, name: ak::Span<'_, u8>) -> u64;
    fn web_bindings_set_event_handler_attribute(event_target: *mut c_void, name: ak::Span<'_, u8>, value: u64);
    fn web_bindings_create_event(
        realm: *mut c_void,
        r#type: ak::Span<'_, u8>,
        bubbles: bool,
        cancelable: bool,
        composed: bool,
    ) -> *mut c_void;
    fn web_bindings_create_rust_event(
        realm: *mut c_void,
        r#type: usize,
        bubbles: bool,
        cancelable: bool,
        composed: bool,
    ) -> *mut c_void;
    fn web_bindings_set_event_owner(event: *mut c_void, interface_name: u16, owner: *mut c_void);
    fn web_bindings_dispatch_event(target: *mut c_void, event: *mut c_void) -> bool;
    fn web_bindings_dispatch_event_with_legacy_output_did_listeners_throw_flag(
        target: *mut c_void,
        event: *mut c_void,
    ) -> bool;
    fn web_bindings_event_canceled(event: *mut c_void) -> bool;
}

/// The EventTarget that an object owns.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppEventTarget(CppCell);

/// https://dom.spec.whatwg.org/#get-the-parent
pub(crate) trait GetTheParent: Interface {
    fn get_the_parent(&self) -> Option<CppEventTarget>;
}

impl CppEventTarget {
    pub(crate) fn new() -> Self {
        // SAFETY: LibWeb returns a new, live EventTarget.
        Self(unsafe { CppCell::from_raw(web_bindings_create_event_target()) })
    }

    pub(crate) fn set_owner<T: GetTheParent>(&self, owner: Ref<T>) {
        unsafe extern "C" fn get_the_parent<T: GetTheParent>(owner: *mut c_void) -> *mut c_void {
            // SAFETY: LibWeb passes the owner that set_owner() gave it, which is the live cell of an object of T.
            let owner = unsafe { cell_from_abi::<T>(owner) };
            owner
                .get_the_parent()
                .map_or(core::ptr::null_mut(), |parent| parent.as_ptr())
        }
        // SAFETY: The EventTarget and the owner are live, and the owner keeps the EventTarget alive.
        unsafe {
            web_bindings_set_event_target_owner(
                self.as_ptr(),
                T::INTERFACE_NAME,
                cell_into_abi(owner),
                get_the_parent::<T>,
            );
        }
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://dom.spec.whatwg.org/#concept-event-dispatch
    pub(crate) fn dispatch_event(&self, event: &CppEvent) -> bool {
        // SAFETY: The EventTarget and the event are live.
        unsafe { web_bindings_dispatch_event(self.as_ptr(), event.as_ptr()) }
    }

    /// Returns the legacyOutputDidListenersThrowFlag.
    pub(crate) fn dispatch_event_with_legacy_output_did_listeners_throw_flag(&self, event: &CppEvent) -> bool {
        // SAFETY: The EventTarget and the event are live.
        unsafe {
            web_bindings_dispatch_event_with_legacy_output_did_listeners_throw_flag(self.as_ptr(), event.as_ptr())
        }
    }
}

/// An Event: a plain one, or the one that an object owns.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppEvent(CppCell);

impl CppEvent {
    pub(crate) fn create(realm: Gc<Realm>, r#type: &str, bubbles: bool, cancelable: bool, composed: bool) -> Self {
        // SAFETY: The realm is live, the type is lent for the call, and LibWeb returns a live event.
        Self(unsafe {
            CppCell::from_raw(web_bindings_create_event(
                realm.as_ptr().cast(),
                ak::Span::new(r#type.as_bytes()),
                bubbles,
                cancelable,
                composed,
            ))
        })
    }

    pub(crate) fn create_for_owner(
        realm: Gc<Realm>,
        r#type: &Utf16String,
        bubbles: bool,
        cancelable: bool,
        composed: bool,
    ) -> Self {
        // SAFETY: The realm is live, the type is lent for the call, and LibWeb returns a live event.
        Self(unsafe {
            CppCell::from_raw(web_bindings_create_rust_event(
                realm.as_ptr().cast(),
                r#type.raw_identity(),
                bubbles,
                cancelable,
                composed,
            ))
        })
    }

    pub(crate) fn set_owner<T: Interface>(&self, owner: Ref<T>) {
        // SAFETY: The event and the owner are live, and the owner keeps the event alive.
        unsafe { web_bindings_set_event_owner(self.as_ptr(), T::INTERFACE_NAME, cell_into_abi(owner)) };
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// https://dom.spec.whatwg.org/#canceled-flag
    pub(crate) fn canceled(&self) -> bool {
        // SAFETY: The event is live.
        unsafe { web_bindings_event_canceled(self.as_ptr()) }
    }
}

fn event_target_of(object: &impl Wrappable) -> *mut c_void {
    object
        .cpp_parent()
        .expect("an object with event handler IDL attributes owns an EventTarget")
        .as_ptr()
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-idl-attributes
pub(crate) fn event_handler_attribute(object: &impl Wrappable, event_type: &str) -> Value {
    // SAFETY: The EventTarget is live, and the name is lent for the call.
    let value =
        unsafe { web_bindings_event_handler_attribute(event_target_of(object), ak::Span::new(event_type.as_bytes())) };
    // SAFETY: LibWeb returns a live value.
    unsafe { Value::from_raw(value) }
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-idl-attributes
pub(crate) fn set_event_handler_attribute(object: &impl Wrappable, event_type: &str, value: Value) {
    // SAFETY: The EventTarget and the value are live, and the name is lent for the call.
    unsafe {
        web_bindings_set_event_handler_attribute(
            event_target_of(object),
            ak::Span::new(event_type.as_bytes()),
            value.to_raw(),
        );
    }
}

impl CppEventTarget {
    pub(crate) fn as_cpp_parent(&self) -> Option<NonNull<c_void>> {
        NonNull::new(self.as_ptr())
    }
}

impl CppEvent {
    pub(crate) fn as_cpp_parent(&self) -> Option<NonNull<c_void>> {
        NonNull::new(self.as_ptr())
    }
}
