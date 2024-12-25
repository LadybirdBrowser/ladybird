/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/StorageAPI/StorageShelf.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#create-a-storage-shelf
StorageShelf::StorageShelf(StorageType type)
{
    // 1. Let shelf be a new storage shelf.
    // 2. Set shelf’s bucket map["default"] to the result of running create a storage bucket with type.
    bucket_map.set("default"_string, StorageBucket { type });
    // 3. Return shelf.
}

}
