/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/StorageAPI/StorageType.h>
#include <LibWebView/StorageOperationError.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-bottle
class StorageBottle : public GC::Cell {
    GC_CELL(StorageBottle, GC::Cell);

public:
    static GC::Ref<StorageBottle> create(GC::Heap& heap, GC::Ref<Page> page, StorageType type, StorageKey const& key, Optional<u64> quota);

    virtual ~StorageBottle() = default;

    // A storage bottle also has a proxy map reference set, which is initially an empty set
    GC::Ref<StorageBottle> proxy() { return *this; }

    virtual size_t size() const = 0;
    virtual Vector<String> keys() const = 0;
    virtual Optional<String> get(String const&) const = 0;
    virtual WebView::StorageOperationError set(String const& key, String const& value) = 0;
    virtual void clear() = 0;
    virtual void remove(String const&) = 0;

    Optional<u64> quota() const { return m_quota; }

protected:
    explicit StorageBottle(Optional<u64> quota)
        : m_quota(quota)
    {
    }

    Optional<u64> m_quota;
};

class LocalStorageBottle final : public StorageBottle {
    GC_CELL(LocalStorageBottle, StorageBottle);
    GC_DECLARE_ALLOCATOR(LocalStorageBottle);

public:
    static GC::Ref<LocalStorageBottle> create(GC::Heap& heap, GC::Ref<Page> page, StorageKey const& key, Optional<u64> quota)
    {
        return heap.allocate<LocalStorageBottle>(page, key, quota);
    }

    virtual size_t size() const override;
    virtual Vector<String> keys() const override;
    virtual Optional<String> get(String const&) const override;
    virtual WebView::StorageOperationError set(String const& key, String const& value) override;
    virtual void clear() override;
    virtual void remove(String const&) override;

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    explicit LocalStorageBottle(GC::Ref<Page> page, StorageKey key, Optional<u64> quota)
        : StorageBottle(quota)
        , m_page(move(page))
        , m_storage_key(move(key))
    {
    }

    GC::Ref<Page> m_page;
    StorageKey m_storage_key;
};

class SessionStorageBottle final : public StorageBottle {
    GC_CELL(SessionStorageBottle, StorageBottle);
    GC_DECLARE_ALLOCATOR(SessionStorageBottle);

public:
    static GC::Ref<SessionStorageBottle> create(GC::Heap& heap, Optional<u64> quota)
    {
        return heap.allocate<SessionStorageBottle>(quota);
    }

    virtual size_t size() const override;
    virtual Vector<String> keys() const override;
    virtual Optional<String> get(String const&) const override;
    virtual WebView::StorageOperationError set(String const& key, String const& value) override;
    virtual void clear() override;
    virtual void remove(String const&) override;

private:
    explicit SessionStorageBottle(Optional<u64> quota)
        : StorageBottle(quota)
    {
    }

    // A storage bottle has a map, which is initially an empty map
    OrderedHashMap<String, String> m_map;
};

using BottleMap = Array<GC::Ptr<StorageBottle>, to_underlying(StorageEndpointType::Count)>;

// https://storage.spec.whatwg.org/#storage-bucket
// A storage bucket is a place for storage endpoints to store data.
class StorageBucket : public GC::Cell {
    GC_CELL(StorageBucket, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageBucket);

public:
    static GC::Ref<StorageBucket> create(GC::Heap& heap, GC::Ref<Page> page, StorageKey const& key, StorageType type) { return heap.allocate<StorageBucket>(page, key, type); }

    BottleMap& bottle_map() { return m_bottle_map; }
    BottleMap const& bottle_map() const { return m_bottle_map; }

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    explicit StorageBucket(GC::Ref<Page> page, StorageKey const& key, StorageType type);

    // A storage bucket has a bottle map of storage identifiers to storage bottles.
    BottleMap m_bottle_map;
};

GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);
GC::Ptr<StorageBottle> obtain_a_storage_bottle_map(StorageType, HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);

}
