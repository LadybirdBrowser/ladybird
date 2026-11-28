/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage shelves, https://storage.spec.whatwg.org/#storage-shelf

use core::ffi::c_void;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Vm};

use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;
use crate::storage_api::storage_bottle::{StorageBucket, StorageType};
use crate::storage_api::storage_key::obtain_a_storage_key;

unsafe extern "C" {
    fn web_storage_api_storage_usage(environment: *mut c_void) -> u64;
}

// https://storage.spec.whatwg.org/#storage-shelf
// A storage shelf exists for each storage key within a storage shed. It holds a bucket map, which is a map of strings to storage buckets.
type BucketMap = Vec<(String, Gc<EmbedderCell<StorageBucket>>)>;

#[derive(Trace)]
pub(crate) struct StorageShelf {
    environment: CppEnvironmentSettingsObject,
    bucket_map: BucketMap,
}

static STORAGE_SHELF_CLASS: EmbedderClass<StorageShelf> = EmbedderClass::new("StorageShelf");

impl EmbedderValue for StorageShelf {
    const CLASS: &'static EmbedderClass<Self> = &STORAGE_SHELF_CLASS;
}

impl StorageShelf {
    // https://storage.spec.whatwg.org/#create-a-storage-shelf
    fn create(
        vm: &Vm,
        environment: &CppEnvironmentSettingsObject,
        storage_type: StorageType,
    ) -> Gc<EmbedderCell<Self>> {
        // 1. Let shelf be a new storage shelf.
        STORAGE_SHELF_CLASS.allocate(vm, |_| {
            // 2. Set shelf’s bucket map["default"] to the result of running create a storage bucket with type.
            let bucket_map = vec![("default".to_owned(), StorageBucket::create(vm, storage_type))];
            // 3. Return shelf.
            StorageShelf {
                environment: environment.clone(),
                bucket_map,
            }
        })
    }

    /// The bucket map[key], which exists.
    pub(crate) fn bucket(&self, key: &str) -> Gc<EmbedderCell<StorageBucket>> {
        self.bucket_map
            .iter()
            .find_map(|(name, bucket)| (name == key).then_some(*bucket))
            .expect("the shelf has the bucket")
    }

    /// https://storage.spec.whatwg.org/#storage-usage
    pub(crate) fn storage_usage(&self) -> u64 {
        // The storage usage of a storage shelf is an implementation-defined rough estimate of the amount of bytes used
        // by it.
        // SAFETY: The environment settings object is live.
        unsafe { web_storage_api_storage_usage(self.environment.as_ptr()) }
    }

    /// https://storage.spec.whatwg.org/#storage-quota
    #[allow(clippy::unused_self, reason = "every shelf has the same quota")]
    pub(crate) fn storage_quota(&self) -> u64 {
        // "The storage quota of a storage shelf is an implementation-defined conservative estimate of the total amount of
        //  bytes it can hold. This amount should be less than the total storage space on the device. It must not be a
        //  function of the available storage space on the device."
        // The bottles with a quota of their own (localStorage, sessionStorage) hold a few MiB each, and the rest (Cache
        // Storage, IndexedDB, service worker registrations) are unbounded, so the shelf reports the 10 GiB a site may
        // reasonably put in those. Nothing about the device goes into the figure.
        10 * 1024 * 1024 * 1024
    }
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-shelf
fn obtain_a_storage_shelf(
    vm: &Vm,
    environment: &CppEnvironmentSettingsObject,
    storage_type: StorageType,
) -> Option<Gc<EmbedderCell<StorageShelf>>> {
    // 1. Let key be the result of running obtain a storage key with environment.
    // 2. If key is failure, then return failure.
    let _key = obtain_a_storage_key(environment)?;

    // 3. If shed[key] does not exist, then set shed[key] to the result of running create a storage shelf with type.
    // 4. Return shed[key].
    // AD-HOC: The user agent's storage shed is kept by the browser process, in a StorageJar. This shelf is a transient
    //         helper for the StorageManager methods, which read its usage, quota and default bucket's mode. Nothing
    //         sets a bucket's mode yet, so a standalone shelf is functionally equivalent to the spec's requirement to
    //         obtain one from a shed.
    Some(StorageShelf::create(vm, environment, storage_type))
}

/// https://storage.spec.whatwg.org/#obtain-a-local-storage-shelf
pub(crate) fn obtain_a_local_storage_shelf(
    vm: &Vm,
    environment: &CppEnvironmentSettingsObject,
) -> Option<Gc<EmbedderCell<StorageShelf>>> {
    // To obtain a local storage shelf, given an environment settings object environment, return the result of running
    // obtain a storage shelf with the user agent’s storage shed, environment, and "local".
    obtain_a_storage_shelf(vm, environment, StorageType::Local)
}
