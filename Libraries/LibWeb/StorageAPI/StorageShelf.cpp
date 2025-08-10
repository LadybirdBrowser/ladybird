/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/StorageAPI/StorageShelf.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageShelf);

void StorageShelf::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_bucket_map);
}

// https://storage.spec.whatwg.org/#create-a-storage-shelf
StorageShelf::StorageShelf(GC::Ref<Page> page, StorageKey key, StorageType type)
{
    // 1. Let shelf be a new storage shelf.
    // 2. Set shelf’s bucket map["default"] to the result of running create a storage bucket with type.
    m_bucket_map.set("default"_string, StorageBucket::create(heap(), page, key, type));
    // 3. Return shelf.
}

u64 StorageShelf::storage_usage() const
{
    u64 usage_in_bytes = 0;
    for (auto const& [key, bucket] : m_bucket_map) {
        usage_in_bytes += key.bytes_as_string_view().length();
        for (auto bottle : bucket->bottle_map()) {
            if (!bottle)
                continue;

            usage_in_bytes += bottle->usage();
        }
    }

    return usage_in_bytes;
}

u64 StorageShelf::storage_quota() const
{
    u64 quota_in_bytes = 0;
    for (auto const& [_, bucket] : m_bucket_map) {
        for (auto bottle : bucket->bottle_map()) {
            if (!bottle)
                continue;

            if (bottle->quota().has_value()) {
                quota_in_bytes += *bottle->quota();
            }
        }
    }
    return quota_in_bytes;
}

}
