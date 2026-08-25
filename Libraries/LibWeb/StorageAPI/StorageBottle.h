/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/StorageAPI/StorageType.h>
#include <LibWebView/StorageSetResult.h>

namespace Web::StorageAPI {

using StorageSetResult = Variant<WebView::StorageOperationError, Optional<Utf16String>>;

// The storage map for one endpoint and storage key — shared by every bottle in this process that addresses it. Reads
// are answered from here, rather than from a synchronous round trip to the process that owns the store — so two windows
// of the same origin can't observe different maps.
class CachedStorageMap : public RefCounted<CachedStorageMap>
    , public Weakable<CachedStorageMap> {
public:
    OrderedHashMap<Utf16String, Utf16String> entries;
    // Bytes these entries contribute toward the storage key's quota, maintained the way the owning
    // process maintains it so both agree on when a write does not fit.
    u64 quota_used { 0 };
    bool primed { false };
};

// The cached map for one endpoint and cache key — created empty, and unprimed on first ask. The cache key has to
// partition maps exactly the way the owning process does when it resolves a store. Otherwise, two distinct maps would
// share one cache: local storage is per storage key, while session storage is additionally per-page — because the owner
// resolves it through that page's top-level traversable.
NonnullRefPtr<CachedStorageMap> cached_storage_map(StorageEndpointType, String const& cache_key);
String storage_cache_key(StorageEndpointType, String const& storage_key);

// Drop every cached map for one endpoint and storage key — whatever traversable each was partitioned under.
WEB_API void invalidate_cached_storage_maps(StorageEndpointType, String const& storage_key);

// https://storage.spec.whatwg.org/#storage-bottle
class StorageBottle : public GC::Cell {
    GC_CELL(StorageBottle, GC::Cell);

public:
    static GC::Ref<StorageBottle> create(GC::Ref<Page> page, StorageType type, StorageKey key, Optional<u64> quota);

    virtual ~StorageBottle() = default;

    // A storage bottle also has a proxy map reference set, which is initially an empty set
    GC::Ref<StorageBottle> proxy() { return *this; }

    virtual size_t size() const = 0;
    virtual Vector<Utf16String> keys() const = 0;
    virtual Optional<Utf16String> get(Utf16View) const = 0;
    virtual StorageSetResult set(Utf16View key, Utf16View value) = 0;
    virtual void clear() = 0;
    virtual void remove(Utf16View) = 0;

    Optional<u64> quota() const { return m_quota; }

protected:
    StorageBottle(Optional<u64> quota, StorageEndpointType endpoint_type, String const& storage_key)
        : m_quota(quota)
        , m_cache(cached_storage_map(endpoint_type, storage_cache_key(endpoint_type, storage_key)))
    {
    }

    Optional<u64> m_quota;
    NonnullRefPtr<CachedStorageMap> m_cache;
};

class LocalStorageBottle final : public StorageBottle {
    GC_CELL(LocalStorageBottle, StorageBottle);
    GC_DECLARE_ALLOCATOR(LocalStorageBottle);

public:
    static GC::Ref<LocalStorageBottle> create(GC::Ref<Page> page, StorageKey key, Optional<u64> quota)
    {
        return GC::Heap::the().allocate<LocalStorageBottle>(page, StorageEndpointType::LocalStorage, key, quota);
    }

    virtual size_t size() const override;
    virtual Vector<Utf16String> keys() const override;
    virtual Optional<Utf16String> get(Utf16View) const override;
    virtual StorageSetResult set(Utf16View key, Utf16View value) override;
    virtual void clear() override;
    virtual void remove(Utf16View) override;

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    void ensure_primed() const;

    explicit LocalStorageBottle(GC::Ref<Page> page, StorageEndpointType endpoint_type, StorageKey key, Optional<u64> quota)
        : StorageBottle(quota, endpoint_type, key.to_string())
        , m_page(move(page))
        , m_endpoint_type(endpoint_type)
        , m_storage_key(move(key))
    {
    }

    GC::Ref<Page> m_page;
    StorageEndpointType m_endpoint_type;
    StorageKey m_storage_key;
};

class SessionStorageBottle final : public StorageBottle {
    GC_CELL(SessionStorageBottle, StorageBottle);
    GC_DECLARE_ALLOCATOR(SessionStorageBottle);

public:
    static GC::Ref<SessionStorageBottle> create(GC::Ref<Page> page, StorageKey key, Optional<u64> quota)
    {
        return GC::Heap::the().allocate<SessionStorageBottle>(page, StorageEndpointType::SessionStorage, move(key), quota);
    }

    virtual size_t size() const override;
    virtual Vector<Utf16String> keys() const override;
    virtual Optional<Utf16String> get(Utf16View) const override;
    virtual StorageSetResult set(Utf16View key, Utf16View value) override;
    virtual void clear() override;
    virtual void remove(Utf16View) override;

    void copy_map_from(SessionStorageBottle const&);

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    void ensure_primed() const;

    explicit SessionStorageBottle(GC::Ref<Page> page, StorageEndpointType endpoint_type, StorageKey key, Optional<u64> quota)
        : StorageBottle(quota, endpoint_type, key.to_string())
        , m_page(move(page))
        , m_endpoint_type(endpoint_type)
        , m_storage_key(move(key))
    {
    }

    GC::Ref<Page> m_page;
    StorageEndpointType m_endpoint_type;
    StorageKey m_storage_key;
};

using BottleMap = Array<GC::Ptr<StorageBottle>, to_underlying(StorageEndpointType::Count)>;

// https://storage.spec.whatwg.org/#storage-bucket
// A storage bucket is a place for storage endpoints to store data.
class StorageBucket : public GC::Cell {
    GC_CELL(StorageBucket, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageBucket);

public:
    static GC::Ref<StorageBucket> create(GC::Ref<Page> page, StorageKey key, StorageType type) { return GC::Heap::the().allocate<StorageBucket>(page, key, type); }

    BottleMap& bottle_map() { return m_bottle_map; }
    BottleMap const& bottle_map() const { return m_bottle_map; }

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    explicit StorageBucket(GC::Ref<Page> page, StorageKey key, StorageType type);

    // A storage bucket has a bottle map of storage identifiers to storage bottles.
    BottleMap m_bottle_map;
};

GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);
GC::Ptr<StorageBottle> obtain_a_storage_bottle_map(StorageType, HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);

}
