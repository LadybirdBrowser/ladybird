/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The services of LibWeb's C++ bindings that the Rust bindings use, which the bindings generator defines in
//! Bindings/IntrinsicDefinitions.cpp.

use core::ffi::c_void;

unsafe extern "C" {
    /// The interface prototype object of the interface in the realm, which the realm's intrinsics create on first use.
    /// The realm and the object are the runtime's cells, and the interface name is a Web::Bindings::InterfaceName of an
    /// interface implemented in Rust.
    pub(crate) fn web_bindings_ensure_web_prototype(realm: *mut c_void, interface_name: u16) -> *mut c_void;
}
