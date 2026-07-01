/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageShed.h>
#include <LibWeb/StorageAPI/StorageShelf.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageShelf);

void StorageShelf::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_bucket_map);
}

// https://storage.spec.whatwg.org/#create-a-storage-shelf
StorageShelf::StorageShelf(GC::Ref<Page> page, StorageKey key, StorageType type)
    : m_page(page)
    , m_key(move(key))
{
    // 1. Let shelf be a new storage shelf.
    // 2. Set shelf’s bucket map["default"] to the result of running create a storage bucket with type.
    m_bucket_map.set("default"_string, StorageBucket::create(heap(), page, m_key, type));
    // 3. Return shelf.
}

u64 StorageShelf::storage_usage() const
{
    return m_page->client().page_did_request_storage_usage(m_key.to_string());
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

// https://storage.spec.whatwg.org/#obtain-a-local-storage-shelf
GC::Ptr<StorageShelf> obtain_a_local_storage_shelf(HTML::EnvironmentSettingsObject& settings)
{
    // To obtain a local storage shelf, given an environment settings object environment, return the result of running
    // obtain a storage shelf with the user agent’s storage shed, environment, and "local".

    // FIXME: This should be implemented in a way that works for Workers.
    auto& window = as<HTML::Window>(settings.global_object());
    auto navigable = window.associated_document().navigable();
    if (!navigable || !navigable->traversable_navigable())
        return {};

    auto& shed = navigable->traversable_navigable()->storage_shed();
    return shed.obtain_a_storage_shelf(settings, StorageType::Local);
}

}
