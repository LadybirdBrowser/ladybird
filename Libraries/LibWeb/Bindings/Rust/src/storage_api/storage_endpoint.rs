/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage endpoints, https://storage.spec.whatwg.org/#storage-endpoint

use core::ffi::c_void;

use crate::storage_api::storage_area_mirror::StorageEndpointType;
use crate::storage_api::storage_bottle::StorageType;

unsafe extern "C" {
    fn web_storage_api_for_each_registered_endpoint(
        endpoints: *mut c_void,
        on_endpoint: unsafe extern "C" fn(*mut c_void, u8, bool, bool, u64),
    );
}

// https://storage.spec.whatwg.org/#storage-endpoint
//
// A storage endpoint is a local or session storage API that uses the infrastructure defined by this
// standard, most notably storage bottles, to keep track of its storage needs.
pub(crate) struct StorageEndpoint {
    // https://storage.spec.whatwg.org/#storage-endpoint-identifier
    // A storage endpoint has an identifier, which is a storage identifier.
    pub(crate) identifier: StorageEndpointType,

    // https://storage.spec.whatwg.org/#storage-endpoint-types
    // A storage endpoint also has types, which is a set of storage types.
    // NOTE: We do not implement this as a set as it is not necessary in the current implementation.
    pub(crate) storage_type: StorageType,

    // https://storage.spec.whatwg.org/#storage-endpoint-quota
    // A storage endpoint also has a quota, which is null or a number representing a recommended quota (in bytes) for each storage bottle corresponding to this storage endpoint.
    pub(crate) quota: Option<u64>,
}

unsafe extern "C" fn on_endpoint(
    endpoints: *mut c_void,
    identifier: u8,
    is_session: bool,
    has_quota: bool,
    quota: u64,
) {
    let identifier = match identifier {
        0 => StorageEndpointType::Caches,
        1 => StorageEndpointType::IndexedDB,
        2 => StorageEndpointType::LocalStorage,
        3 => StorageEndpointType::ServiceWorkerRegistrations,
        4 => StorageEndpointType::SessionStorage,
        _ => panic!("LibWeb passes a registered storage endpoint"),
    };
    // SAFETY: endpoints is the vector of the caller, which outlives the call.
    unsafe { &mut *endpoints.cast::<Vec<StorageEndpoint>>() }.push(StorageEndpoint {
        identifier,
        storage_type: if is_session {
            StorageType::Session
        } else {
            StorageType::Local
        },
        quota: has_quota.then_some(quota),
    });
}

impl StorageEndpoint {
    /// https://storage.spec.whatwg.org/#registered-storage-endpoints
    pub(crate) fn registered_endpoints() -> Vec<Self> {
        let mut endpoints = Vec::new();
        // SAFETY: The vector outlives the call, which copies the registered endpoints from LibWebCommon.
        unsafe { web_storage_api_for_each_registered_endpoint((&raw mut endpoints).cast(), on_endpoint) };
        endpoints
    }
}
