/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageType.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-shelf
// A storage shelf exists for each storage key within a storage shed. It holds a bucket map, which is a map of strings to storage buckets.
using BucketMap = OrderedHashMap<String, GC::Ref<StorageBucket>>;

class StorageShelf : public GC::Cell {
    GC_CELL(StorageShelf, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageShelf);

public:
    static GC::Ref<StorageShelf> create(GC::Heap& heap, GC::Ref<Page> page, StorageKey const& key, StorageType type) { return heap.allocate<StorageShelf>(page, key, type); }

    BucketMap& bucket_map() { return m_bucket_map; }
    BucketMap const& bucket_map() const { return m_bucket_map; }

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    explicit StorageShelf(GC::Ref<Page>, StorageKey const&, StorageType);

    BucketMap m_bucket_map;
};

}
