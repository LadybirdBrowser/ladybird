/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/StorageAPI/StorageEndpoint.h>

namespace Web::StorageAPI {

ReadonlySpan<StorageEndpoint> StorageEndpoint::registered_endpoints()
{
    // https://storage.spec.whatwg.org/#registered-storage-endpoints
    static auto const endpoints = to_array<StorageEndpoint>({
        { "caches"_string, StorageType::Local, {} },
        { "indexedDB"_string, StorageType::Local, {} },
        { "localStorage"_string, StorageType::Local, 5 * MiB },
        { "serviceWorkerRegistrations"_string, StorageType::Local, {} },
        { "sessionStorage"_string, StorageType::Session, 5 * MiB },
    });
    return endpoints;
}

}
