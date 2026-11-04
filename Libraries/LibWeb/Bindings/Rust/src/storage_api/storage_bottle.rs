/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage bottles, https://storage.spec.whatwg.org/#storage-bottle

use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Visitor, Vm};

use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;
use crate::bindings::global_object::window_from_global_object;
pub(crate) use crate::storage_api::storage_area_mirror::QuotaExceeded;
use crate::storage_api::storage_area_mirror::{Requester, StorageAreaMirror, StorageEndpointType};
use crate::storage_api::storage_endpoint::StorageEndpoint;
use crate::storage_api::storage_key::{StorageKey, obtain_a_storage_key};

/// https://storage.spec.whatwg.org/#storage-type
/// A storage type is "local" or "session".
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum StorageType {
    Local,
    Session,
}

/// https://storage.spec.whatwg.org/#storage-bottle
///
/// NB: The bottle's map lives in the browser process; what a bottle here holds is this process's mirror of it, shared
///     with every other bottle in the process for the same area. Each bottle object is one storage proxy map over it.
pub(crate) struct StorageBottle {
    environment: CppEnvironmentSettingsObject,
    environment_id: String,
    page_id: u64,
    // https://storage.spec.whatwg.org/#storage-bottle-quota
    // A storage bottle also has a quota, which is null or a number representing a conservative estimate of the total
    // amount of bytes it can hold. Null indicates the lack of a limit.
    quota: Option<u64>,
    mirror: Rc<StorageAreaMirror>,
}

// SAFETY: The settings object is the only cell a bottle holds.
unsafe impl Trace for StorageBottle {
    fn trace(&self, visitor: &mut Visitor) {
        self.environment.trace(visitor);
    }
}

impl StorageBottle {
    fn create(environment: &CppEnvironmentSettingsObject, endpoint: &StorageEndpoint, key: &StorageKey) -> Self {
        let window =
            window_from_global_object(environment.global_object()).expect("a storage bottle map is a Window's");
        let page_id = window.page_id();
        // Session storage is partitioned by the traversable, and a page's documents all belong to one traversable, so
        // a session bottle's mirror is found by its page as well as its storage key.
        let session_page = (endpoint.identifier == StorageEndpointType::SessionStorage).then_some(page_id);
        Self {
            environment: environment.clone(),
            environment_id: environment.id(),
            page_id,
            quota: endpoint.quota,
            mirror: StorageAreaMirror::obtain(endpoint.identifier, session_page, key, endpoint.quota),
        }
    }

    fn requester(&self) -> Requester<'_> {
        Requester {
            page_id: self.page_id,
            environment_id: &self.environment_id,
            environment: self.environment.as_ptr(),
        }
    }

    /// Loads the map now rather than on first access, so changes other processes make start arriving.
    pub(crate) fn connect(&self) {
        self.mirror.connect(&self.requester());
    }

    pub(crate) fn size(&self) -> usize {
        self.mirror.size(&self.requester())
    }

    /// https://infra.spec.whatwg.org/#map-getting-the-keys
    pub(crate) fn keys(&self) -> Vec<Utf16String> {
        self.mirror.keys(&self.requester())
    }

    pub(crate) fn get(&self, key: &Utf16String) -> Option<Utf16String> {
        self.mirror.get(&self.requester(), key)
    }

    /// The value the key had, or the quota's refusal.
    pub(crate) fn set(
        &self,
        key: &Utf16String,
        value: &Utf16String,
        document_url: &str,
    ) -> Result<Option<Utf16String>, QuotaExceeded> {
        self.mirror.set(&self.requester(), key, value, document_url)
    }

    pub(crate) fn remove(&self, key: &Utf16String, document_url: &str) {
        let _ = self.mirror.remove(&self.requester(), key, document_url);
    }

    pub(crate) fn clear(&self, document_url: &str) {
        self.mirror.clear(&self.requester(), document_url);
    }

    pub(crate) fn quota(&self) -> Option<u64> {
        self.quota
    }
}

type BottleMap = [Option<StorageBottle>; StorageEndpointType::COUNT];

// https://storage.spec.whatwg.org/#storage-bucket
// A storage bucket is a place for storage endpoints to store data.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StorageBucket {
    // A storage bucket has a bottle map of storage identifiers to storage bottles.
    bottle_map: BottleMap,
}

static STORAGE_BUCKET_CLASS: EmbedderClass<StorageBucket> = EmbedderClass::new("StorageBucket");

impl EmbedderValue for StorageBucket {
    const CLASS: &'static EmbedderClass<Self> = &STORAGE_BUCKET_CLASS;
}

impl StorageBucket {
    pub(crate) fn create(
        vm: &Vm,
        environment: &CppEnvironmentSettingsObject,
        key: &StorageKey,
        storage_type: StorageType,
    ) -> Gc<EmbedderCell<Self>> {
        // 1. Let bucket be null.
        // 2. If type is "local", then set bucket to a new local storage bucket.
        // 3. Otherwise:
        //     1. Assert: type is "session".
        //     2. Set bucket to a new session storage bucket.
        STORAGE_BUCKET_CLASS.allocate(vm, |_| {
            let mut bottle_map = core::array::from_fn(|_| None);

            // 4. For each endpoint of registered storage endpoints whose types contain type, set bucket’s bottle map[endpoint’s identifier] to a new storage bottle whose quota is endpoint’s quota.
            for endpoint in StorageEndpoint::registered_endpoints() {
                if endpoint.storage_type == storage_type {
                    bottle_map[endpoint.identifier as usize] = Some(StorageBottle::create(environment, &endpoint, key));
                }
            }

            // 5. Return bucket.
            StorageBucket { bottle_map }
        })
    }
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-bottle-map
fn obtain_a_storage_bottle_map(
    storage_type: StorageType,
    environment: &CppEnvironmentSettingsObject,
    identifier: StorageEndpointType,
) -> Option<StorageBottle> {
    // 1. Let shed be null.
    // 2. If type is "local", then set shed to the user agent’s storage shed.
    // 3. Otherwise:
    //     1. Assert: type is "session".
    //     2. Set shed to environment’s global object’s associated Document’s node navigable’s traversable navigable’s storage shed.
    // NB: Both sheds are kept by the browser process, in a StorageJar, which the bottle returned here mirrors. The
    //     bottle names environment, from which the browser process obtains the storage key itself.

    // 4. Let shelf be the result of running obtain a storage shelf, with shed, environment, and type.
    // 5. If shelf is failure, then return failure.
    let storage_key = obtain_a_storage_key(environment)?;

    // 6. Let bucket be shelf’s bucket map["default"].
    // 7. Let bottle be bucket’s bottle map[identifier].
    // NB: The bucket's bottle map holds a bottle for each registered storage endpoint whose types contain type, with
    //     that endpoint's quota.
    let endpoint = StorageEndpoint::registered_endpoints()
        .into_iter()
        .find(|endpoint| endpoint.identifier == identifier)
        .expect("the storage endpoint is registered");
    assert!(
        endpoint.storage_type == storage_type,
        "the endpoint supports the storage type"
    );

    // 8. Let proxyMap be a new storage proxy map whose backing map is bottle’s map.
    // 9. Append proxyMap to bottle’s proxy map reference set.
    // 10. Return proxyMap.
    Some(StorageBottle::create(environment, &endpoint, &storage_key))
}

/// https://storage.spec.whatwg.org/#obtain-a-local-storage-bottle-map
pub(crate) fn obtain_a_local_storage_bottle_map(
    environment: &CppEnvironmentSettingsObject,
    identifier: StorageEndpointType,
) -> Option<StorageBottle> {
    // To obtain a local storage bottle map, given an environment settings object environment and storage identifier
    // identifier, return the result of running obtain a storage bottle map with "local", environment, and identifier.
    obtain_a_storage_bottle_map(StorageType::Local, environment, identifier)
}

/// https://storage.spec.whatwg.org/#obtain-a-session-storage-bottle-map
pub(crate) fn obtain_a_session_storage_bottle_map(
    environment: &CppEnvironmentSettingsObject,
    identifier: StorageEndpointType,
) -> Option<StorageBottle> {
    // To obtain a session storage bottle map, given an environment settings object environment and storage identifier
    // identifier, return the result of running obtain a storage bottle map with "session", environment, and
    // identifier.
    obtain_a_storage_bottle_map(StorageType::Session, environment, identifier)
}
