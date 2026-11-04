/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of the Storage Standard's interfaces implemented in Rust, which LibWeb's C++ code calls.

use core::ffi::c_void;

use super::storage_manager::StorageManager;
use crate::bindings::interface::{cell_into_abi, realm_arguments};

/// A new StorageManager in the realm, the StorageManager object of the realm's settings object.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_storage_api_create_storage_manager(
    vm: *mut c_void,
    realm: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(StorageManager::create(vm, realm))
}
