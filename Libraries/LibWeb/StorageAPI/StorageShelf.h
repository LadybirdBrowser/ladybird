/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageType.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-shelf
// A storage shelf exists for each storage key within a storage shed. It holds a bucket map, which is a map of strings to storage buckets.
using BucketMap = OrderedHashMap<String, StorageBucket>;

struct StorageShelf {
    explicit StorageShelf(StorageType);

    BucketMap bucket_map;
};

}
