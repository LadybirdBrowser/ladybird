/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage keys, https://storage.spec.whatwg.org/#storage-keys

#[cfg(not(test))]
use core::ffi::c_void;

#[cfg(not(test))]
use liburl_rust::FfiOrigin;
use liburl_rust::origin::Origin;

#[cfg(not(test))]
use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;

#[cfg(not(test))]
unsafe extern "C" {
    fn web_storage_api_obtain_a_storage_key(
        environment: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    ) -> bool;
    fn web_storage_api_obtain_a_storage_key_for_non_storage_purposes(
        environment: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    );
}

// https://storage.spec.whatwg.org/#storage-keys
#[derive(Clone)]
pub(crate) struct StorageKey {
    // A storage key is a tuple consisting of an origin (an origin). [HTML]
    // NOTE: This is expected to change; see Client-Side Storage Partitioning https://privacycg.github.io/storage-partitioning/.
    pub(crate) origin: Origin,
}

impl PartialEq for StorageKey {
    fn eq(&self, other: &Self) -> bool {
        // To determine whether a storage key A equals storage key B, run these steps:
        // 1. If A’s origin is not same origin with B’s origin, then return false.
        // 2. Return true.
        self.origin.is_same_origin(&other.origin)
    }
}

#[cfg(not(test))]
unsafe extern "C" fn on_origin(ctx: *mut c_void, ffi_origin: *const FfiOrigin) {
    // SAFETY: ctx is the origin of the caller, and LibWeb lends the origin for the call.
    unsafe { *ctx.cast::<Option<Origin>>() = Some((*ffi_origin).to_origin()) };
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-key
#[cfg(not(test))]
pub(crate) fn obtain_a_storage_key(environment: &CppEnvironmentSettingsObject) -> Option<StorageKey> {
    let mut origin: Option<Origin> = None;
    // SAFETY: The environment settings object is live, and the origin outlives the call.
    unsafe { web_storage_api_obtain_a_storage_key(environment.as_ptr(), (&raw mut origin).cast(), on_origin) };
    origin.map(|origin| StorageKey { origin })
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-key-for-non-storage-purposes
#[cfg(not(test))]
pub(crate) fn obtain_a_storage_key_for_non_storage_purposes(environment: &CppEnvironmentSettingsObject) -> StorageKey {
    let mut origin: Option<Origin> = None;
    // SAFETY: The environment settings object is live, and the origin outlives the call.
    unsafe {
        web_storage_api_obtain_a_storage_key_for_non_storage_purposes(
            environment.as_ptr(),
            (&raw mut origin).cast(),
            on_origin,
        );
    }
    StorageKey {
        origin: origin.expect("LibWeb lends the origin of the storage key"),
    }
}
