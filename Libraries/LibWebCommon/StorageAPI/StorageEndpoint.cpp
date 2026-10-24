/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/StorageAPI/StorageEndpoint.h>

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

Optional<u64> quota_for(StorageEndpointType identifier)
{
    for (auto const& endpoint : StorageEndpoint::registered_endpoints()) {
        if (endpoint.identifier == identifier)
            return endpoint.quota;
    }
    return {};
}

u64 storage_quota_size(Utf16View const& string)
{
    // Counted without encoding: the bytes UTF-8 would take, with a lone surrogate counted as the three bytes it is
    // written as. Every write measures its key and value this way, so building the UTF-8 string would cost a copy of
    // each on the path meant to be fast.
    if (string.has_ascii_storage())
        return string.length_in_code_units();

    u64 size = 0;
    for (auto code_point : string) {
        if (code_point < 0x80)
            size += 1;
        else if (code_point < 0x800)
            size += 2;
        else if (code_point < 0x10000)
            size += 3;
        else
            size += 4;
    }
    return size;
}

}
