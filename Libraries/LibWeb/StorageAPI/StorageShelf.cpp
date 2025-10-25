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
StorageShelf::StorageShelf(GC::Ref<Page> page, StorageKey const& key, StorageType type)
{
    // 1. Let shelf be a new storage shelf.
    // 2. Set shelf’s bucket map["default"] to the result of running create a storage bucket with type.
    m_bucket_map.set("default"_string, StorageBucket::create(heap(), page, move(key), type));
    // 3. Return shelf.
}

}
