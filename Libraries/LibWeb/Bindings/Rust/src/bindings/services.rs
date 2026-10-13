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

    /// The wrapper world of the realm, a cell that the realm keeps alive, and whether it is the main world of its
    /// agent.
    pub(crate) fn web_bindings_wrapper_world(realm: *mut c_void, is_main_world: *mut bool) -> *mut c_void;

    /// The wrapper that the world of the realm, which is not a main world, holds for the cell of an object of an
    /// interface implemented in Rust, or null.
    pub(crate) fn web_bindings_rust_wrapper_for(realm: *mut c_void, cell: *mut c_void) -> *mut c_void;

    /// Makes `wrapper` the wrapper that the world of its realm, which is not a main world, holds for the cell.
    pub(crate) fn web_bindings_set_rust_wrapper(realm: *mut c_void, cell: *mut c_void, wrapper: *mut c_void);

    /// Whether the object is a wrapper of an object that implements the interface, a Web::Bindings::InterfaceName of
    /// an interface that LibWeb implements in C++.
    pub(crate) fn web_bindings_object_implements_interface(object: *mut c_void, interface_name: u16) -> bool;

    /// A promise rejected with the reason, in the realm of the this value of an operation that ran in the current
    /// realm: the realm of the object, or the current realm for a value that is not an object.
    pub(crate) fn web_bindings_rejected_promise(
        current_realm: *mut c_void,
        this_value: u64,
        reason: u64,
    ) -> *mut c_void;
}
