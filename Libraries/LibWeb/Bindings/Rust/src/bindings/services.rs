/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The services of LibWeb's C++ bindings that the Rust bindings use, which the bindings generator defines in
//! Bindings/IntrinsicDefinitions.cpp and Bindings/RustBindingsServices.cpp defines by hand.

use core::ffi::c_void;

unsafe extern "C" {
    /// The interface prototype object of the interface in the realm, which the realm's intrinsics create on first use.
    /// The realm and the object are the runtime's cells, and the interface name is a Web::Bindings::InterfaceName of an
    /// interface implemented in Rust.
    pub(crate) fn web_bindings_ensure_web_prototype(realm: *mut c_void, interface_name: u16) -> *mut c_void;

    /// The interface object of the interface in the realm, likewise.
    pub(crate) fn web_bindings_ensure_web_constructor(realm: *mut c_void, interface_name: u16) -> *mut c_void;

    /// The iterator prototype object of the interface in the realm, likewise, for an interface with a pair iterator.
    pub(crate) fn web_bindings_ensure_web_iterator_prototype(realm: *mut c_void, interface_name: u16) -> *mut c_void;

    /// The wrapper world of the realm, a cell that the realm keeps alive, and whether it is the main world of its
    /// agent.
    pub(crate) fn web_bindings_wrapper_world(realm: *mut c_void, is_main_world: *mut bool) -> *mut c_void;

    /// The wrapper that the world of the realm, which is not a main world, holds for the cell of an object of an
    /// interface implemented in Rust, or null.
    pub(crate) fn web_bindings_rust_wrapper_for(realm: *mut c_void, cell: *mut c_void) -> *mut c_void;

    /// Makes `wrapper` the wrapper that the world of its realm, which is not a main world, holds for the cell.
    pub(crate) fn web_bindings_set_rust_wrapper(realm: *mut c_void, cell: *mut c_void, wrapper: *mut c_void);

    /// Whether the global object of the realm is a Window.
    pub(crate) fn web_bindings_realm_is_window(realm: *mut c_void) -> bool;

    /// Runs the steps with the context while an execution context of the realm is on the stack, as an
    /// HTML::TemporaryExecutionContext puts it there, with callbacks enabled if `callbacks_enabled`.
    pub(crate) fn web_bindings_with_temporary_execution_context(
        realm: *mut c_void,
        callbacks_enabled: bool,
        context: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void),
    );

    /// Whether the object is a wrapper of an object that implements the interface, a Web::Bindings::InterfaceName of
    /// an interface that LibWeb implements in C++.
    pub(crate) fn web_bindings_object_implements_interface(object: *mut c_void, interface_name: u16) -> bool;

    /// Runs the extract an origin operation of the object, https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin,
    /// if it is a platform object of an interface implemented in C++ or a WindowProxy, and calls `on_origin` with the
    /// origin, which it lends for the call, if that is not null. Returns whether it called `on_origin`.
    pub(crate) fn web_bindings_extract_an_origin(
        object: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const liburl_rust::FfiOrigin),
    ) -> bool;

    /// The wrapper in the realm of a new DOMException with the name, which is ASCII, and the message, a borrowed
    /// AK::Utf16String.
    pub(crate) fn web_bindings_create_dom_exception(
        realm: *mut c_void,
        name: ak::Span<'_, u8>,
        message: usize,
    ) -> *mut c_void;

    /// A promise rejected with the reason, in the realm of the this value of an operation that ran in the current
    /// realm: the realm of the object, or the current realm for a value that is not an object.
    pub(crate) fn web_bindings_rejected_promise(
        current_realm: *mut c_void,
        this_value: u64,
        reason: u64,
    ) -> *mut c_void;
}
