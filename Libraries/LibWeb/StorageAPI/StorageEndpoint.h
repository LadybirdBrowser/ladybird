/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibWeb/StorageAPI/StorageType.h>

namespace Web::StorageAPI {

enum class StorageEndpointType : u8 {
    Caches = 0,
    IndexedDB = 1,
    LocalStorage = 2,
    ServiceWorkerRegistrations = 3,
    SessionStorage = 4,

    Count = 5 // This should always be the last value in the enum.
};

// https://storage.spec.whatwg.org/#storage-endpoint
//
// A storage endpoint is a local or session storage API that uses the infrastructure defined by this
// standard, most notably storage bottles, to keep track of its storage needs.
struct StorageEndpoint {
    static constexpr u64 LOCAL_STORAGE_QUOTA = 5 * MiB;
    static constexpr u64 SESSION_STORAGE_QUOTA = 5 * MiB;

    // https://storage.spec.whatwg.org/#storage-endpoint-identifier
    // A storage endpoint has an identifier, which is a storage identifier.
    StorageEndpointType identifier;

    // https://storage.spec.whatwg.org/#storage-endpoint-types
    // A storage endpoint also has types, which is a set of storage types.
    // NOTE: We do not implement this as a set as it is not necessary in the current implementation.
    StorageType type;

    // https://storage.spec.whatwg.org/#storage-endpoint-quota
    // A storage endpoint also has a quota, which is null or a number representing a recommended quota (in bytes) for each storage bottle corresponding to this storage endpoint.
    Optional<u64> quota;

    static ReadonlySpan<StorageEndpoint> registered_endpoints();
};

}
