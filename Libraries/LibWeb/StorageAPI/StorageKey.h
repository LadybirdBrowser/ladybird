/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Traits.h>
#include <LibURL/Origin.h>
#include <LibWeb/Forward.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-keys
struct StorageKey {

    // A storage key is a tuple consisting of an origin (an origin). [HTML]
    // NOTE: This is expected to change; see Client-Side Storage Partitioning https://privacycg.github.io/storage-partitioning/.
    URL::Origin origin;

    friend bool operator==(StorageKey const& a, StorageKey const& b)
    {
        // To determine whether a storage key A equals storage key B, run these steps:
        // 1. If A’s origin is not same origin with B’s origin, then return false.
        // 2. Return true.
        return a.origin.is_same_origin(b.origin);
    }
};

Optional<StorageKey> obtain_a_storage_key(HTML::Environment const&);
StorageKey obtain_a_storage_key_for_non_storage_purposes(HTML::Environment const&);

}

namespace AK {
template<>
struct Traits<Web::StorageAPI::StorageKey> : public DefaultTraits<Web::StorageAPI::StorageKey> {
    static unsigned hash(Web::StorageAPI::StorageKey const& key)
    {
        return Traits<URL::Origin>::hash(key.origin);
    }
};
}
