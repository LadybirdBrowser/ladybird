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
class StorageBottle : public GC::Cell {
    GC_CELL(StorageBottle, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageBottle);

    static GC::Ref<StorageBottle> create(GC::Heap& heap, Optional<u64> quota) { return heap.allocate<StorageBottle>(quota); }

    // A storage bottle has a map, which is initially an empty map
    OrderedHashMap<String, String> map;

    // A storage bottle also has a proxy map reference set, which is initially an empty set
    GC::Ref<StorageBottle> proxy() { return *this; }

    // A storage bottle also has a quota, which is null or a number representing a conservative estimate of
    // the total amount of bytes it can hold. Null indicates the lack of a limit.
    Optional<u64> quota;

private:
    explicit StorageBottle(Optional<u64> quota_)
        : quota(quota_)
    {
    }
};

using BottleMap = OrderedHashMap<String, GC::Ref<StorageBottle>>;

// https://storage.spec.whatwg.org/#storage-bucket
// A storage bucket is a place for storage endpoints to store data.
class StorageBucket : public GC::Cell {
    GC_CELL(StorageBucket, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageBucket);

public:
    static GC::Ref<StorageBucket> create(GC::Heap& heap, StorageType type) { return heap.allocate<StorageBucket>(type); }

    BottleMap& bottle_map() { return m_bottle_map; }
    BottleMap const& bottle_map() const { return m_bottle_map; }

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    explicit StorageBucket(StorageType);

    // A storage bucket has a bottle map of storage identifiers to storage bottles.
    BottleMap m_bottle_map;
};

GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject&, StringView storage_identifier);
GC::Ptr<StorageBottle> obtain_a_local_storage_bottle_map(HTML::EnvironmentSettingsObject&, StringView storage_identifier);
GC::Ptr<StorageBottle> obtain_a_storage_bottle_map(StorageType, HTML::EnvironmentSettingsObject&, StringView storage_identifier);

}
