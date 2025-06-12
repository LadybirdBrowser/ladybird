/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/StorageAPI/StorageEndpoint.h>

namespace Web::StorageAPI {

ReadonlySpan<StorageEndpoint> StorageEndpoint::registered_endpoints()
{
    // https://storage.spec.whatwg.org/#registered-storage-endpoints
    static auto const endpoints = to_array<StorageEndpoint>({
        { StorageEndpointType::Caches, StorageType::Local, {} },
        { StorageEndpointType::IndexedDB, StorageType::Local, {} },
        { StorageEndpointType::LocalStorage, StorageType::Local, LOCAL_STORAGE_QUOTA },
        { StorageEndpointType::ServiceWorkerRegistrations, StorageType::Local, {} },
        { StorageEndpointType::SessionStorage, StorageType::Session, SESSION_STORAGE_QUOTA },
    });
    return endpoints;
}

}
