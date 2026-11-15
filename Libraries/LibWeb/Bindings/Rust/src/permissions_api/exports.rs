/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code reaches the Permissions API, declared in
//! Libraries/LibWeb/PermissionsAPI/Permissions.cpp and PermissionStore.cpp.

use core::ffi::c_void;

use libjs_rust::api::Vm;

use crate::bindings::interface::{allocate, cell_into_abi};
use crate::permissions_api::permission_store::update_statuses_of_this_agent;
use crate::permissions_api::permissions::Permissions;

/// # Safety
///
/// The VM must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_permissions_api_create_permissions(vm: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(allocate(vm, |_| Permissions))
}

/// # Safety
///
/// The VM must be the live VM of the thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_permissions_api_update_statuses_of_this_agent(
    vm: *const c_void,
    revision: u64,
    store_changed: bool,
) -> bool {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    update_statuses_of_this_agent(vm, revision, store_changed)
}
