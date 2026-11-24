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

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_permissions_api_create_permissions(vm: &Vm) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| Permissions))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_permissions_api_update_statuses_of_this_agent(
    vm: &Vm,
    revision: u64,
    store_changed: bool,
) -> bool {
    update_statuses_of_this_agent(vm, revision, store_changed)
}
