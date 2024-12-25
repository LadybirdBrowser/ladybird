/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>
#include <LibWeb/StorageAPI/StorageType.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-bottle
struct StorageBottle : public RefCounted<StorageBottle> {
    static NonnullRefPtr<StorageBottle> create(Optional<u64> quota) { return adopt_ref(*new StorageBottle(quota)); }

    // A storage bottle has a map, which is initially an empty map
    OrderedHashMap<String, String> map;

    // A storage bottle also has a proxy map reference set, which is initially an empty set
    NonnullRefPtr<StorageBottle> proxy() { return *this; }

    // A storage bottle also has a quota, which is null or a number representing a conservative estimate of
    // the total amount of bytes it can hold. Null indicates the lack of a limit.
    Optional<u64> quota;

private:
    explicit StorageBottle(Optional<u64> quota_)
        : quota(quota_)
    {
    }
};

using BottleMap = OrderedHashMap<String, NonnullRefPtr<StorageBottle>>;

// https://storage.spec.whatwg.org/#storage-bucket
// A storage bucket is a place for storage endpoints to store data.
struct StorageBucket {
    explicit StorageBucket(StorageType);

    // A storage bucket has a bottle map of storage identifiers to storage bottles.
    BottleMap bottle_map;
};

RefPtr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject&, StringView storage_identifier);
RefPtr<StorageBottle> obtain_a_local_storage_bottle_map(HTML::EnvironmentSettingsObject&, StringView storage_identifier);
RefPtr<StorageBottle> obtain_a_storage_bottle_map(StorageType, HTML::EnvironmentSettingsObject&, StringView storage_identifier);

}
