/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage shelves, https://storage.spec.whatwg.org/#storage-shelf

use core::ffi::c_void;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Vm};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;

unsafe extern "C" {
    fn web_storage_api_obtain_a_local_storage_shelf(environment: *mut c_void) -> *mut c_void;
    fn web_storage_api_storage_shelf_storage_usage(shelf: *mut c_void) -> u64;
    fn web_storage_api_storage_shelf_storage_quota(shelf: *mut c_void) -> u64;
}

/// A storage shelf of LibWeb's C++ code, StorageAPI::StorageShelf.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StorageShelf(CppCell);

static STORAGE_SHELF_CLASS: EmbedderClass<StorageShelf> = EmbedderClass::new("StorageShelf");

impl EmbedderValue for StorageShelf {
    const CLASS: &'static EmbedderClass<Self> = &STORAGE_SHELF_CLASS;
}

impl StorageShelf {
    /// https://storage.spec.whatwg.org/#storage-usage
    pub(crate) fn storage_usage(&self) -> u64 {
        // SAFETY: The shelf is live.
        unsafe { web_storage_api_storage_shelf_storage_usage(self.0.as_ptr()) }
    }

    /// https://storage.spec.whatwg.org/#storage-quota
    pub(crate) fn storage_quota(&self) -> u64 {
        // SAFETY: The shelf is live.
        unsafe { web_storage_api_storage_shelf_storage_quota(self.0.as_ptr()) }
    }
}

/// https://storage.spec.whatwg.org/#obtain-a-local-storage-shelf
pub(crate) fn obtain_a_local_storage_shelf(
    vm: &Vm,
    environment: &CppEnvironmentSettingsObject,
) -> Option<Gc<EmbedderCell<StorageShelf>>> {
    // SAFETY: The environment settings object is live.
    let shelf = unsafe { web_storage_api_obtain_a_local_storage_shelf(environment.as_ptr()) };
    if shelf.is_null() {
        return None;
    }
    // SAFETY: LibWeb returns a live shelf.
    let shelf = unsafe { CppCell::from_raw(shelf) };
    Some(STORAGE_SHELF_CLASS.allocate(vm, |_| StorageShelf(shelf)))
}
