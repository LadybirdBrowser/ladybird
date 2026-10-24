/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageAreaMirror.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>
#include <LibWebCommon/StorageAPI/StorageType.h>
#include <LibWebCommon/WebView/StorageSetResult.h>

namespace Web::StorageAPI {

using StorageSetResult = Variant<WebView::StorageOperationError, Optional<Utf16String>>;

// https://storage.spec.whatwg.org/#storage-bottle
// NB: The bottle's map lives in the browser process; what a bottle here holds is this process's mirror of it, shared
//     with every other bottle in the process for the same area. Each bottle object is one storage proxy map over it.
class StorageBottle : public GC::Cell {
    GC_CELL(StorageBottle, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageBottle);

public:
    static GC::Ref<StorageBottle> create(GC::Ref<Page>, StorageEndpoint const&, HTML::EnvironmentId, StorageKey const&);

    virtual ~StorageBottle() = default;

    // A storage bottle also has a proxy map reference set, which is initially an empty set
    GC::Ref<StorageBottle> proxy() { return *this; }

    // Connects the mirror now rather than on first access, so changes other processes make start arriving.
    void connect() const;

    size_t size() const;
    Vector<Utf16String> keys() const;
    Optional<Utf16String> get(Utf16View) const;
    // The URL is the document's, carried to the storage events other processes fire for the change.
    StorageSetResult set(Utf16View key, Utf16View value, String const& document_url);
    void clear(String const& document_url);
    void remove(Utf16View, String const& document_url);

    Optional<u64> quota() const { return m_quota; }

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    StorageBottle(GC::Ref<Page>, StorageEndpointType, HTML::EnvironmentId, StorageKey const&, Optional<u64> quota);

    StorageAreaMirror::Requester requester() const;

    GC::Ref<Page> m_page;
    HTML::EnvironmentId m_environment_id;
    Optional<u64> m_quota;
    NonnullRefPtr<StorageAreaMirror> m_mirror;
};

using BottleMap = Array<GC::Ptr<StorageBottle>, to_underlying(StorageEndpointType::Count)>;

// https://storage.spec.whatwg.org/#storage-bucket
// A storage bucket is a place for storage endpoints to store data.
class StorageBucket : public GC::Cell {
    GC_CELL(StorageBucket, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageBucket);

public:
    static GC::Ref<StorageBucket> create(GC::Ref<Page> page, HTML::EnvironmentId environment_id, StorageKey const& key, StorageType type) { return GC::Heap::the().allocate<StorageBucket>(page, move(environment_id), key, type); }

    BottleMap& bottle_map() { return m_bottle_map; }
    BottleMap const& bottle_map() const { return m_bottle_map; }

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    explicit StorageBucket(GC::Ref<Page> page, HTML::EnvironmentId environment_id, StorageKey const& key, StorageType type);

    // A storage bucket has a bottle map of storage identifiers to storage bottles.
    BottleMap m_bottle_map;
};

GC::Ptr<StorageBottle> obtain_a_local_storage_bottle_map(HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);
GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);
GC::Ptr<StorageBottle> obtain_a_storage_bottle_map(StorageType, HTML::EnvironmentSettingsObject&, StorageEndpointType endpoint_type);

}
