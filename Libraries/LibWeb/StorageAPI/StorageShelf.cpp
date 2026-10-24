/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
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
StorageShelf::StorageShelf(GC::Ref<Page> page, HTML::EnvironmentId environment_id, StorageKey const& key, StorageType type)
    : m_page(page)
    , m_environment_id(move(environment_id))
{
    // 1. Let shelf be a new storage shelf.
    // 2. Set shelf’s bucket map["default"] to the result of running create a storage bucket with type.
    m_bucket_map.set("default"_string, StorageBucket::create(page, m_environment_id, key, type));
    // 3. Return shelf.
}

u64 StorageShelf::storage_usage() const
{
    return m_page->client().page_did_request_storage_usage(m_environment_id);
}

// https://storage.spec.whatwg.org/#storage-quota
u64 StorageShelf::storage_quota() const
{
    // "The storage quota of a storage shelf is an implementation-defined conservative estimate of the total amount of
    //  bytes it can hold. This amount should be less than the total storage space on the device. It must not be a
    //  function of the available storage space on the device."
    // The bottles with a quota of their own (localStorage, sessionStorage) hold a few MiB each, and the rest (Cache
    // Storage, IndexedDB, service worker registrations) are unbounded, so the shelf reports the 10 GiB a site may
    // reasonably put in those. Nothing about the device goes into the figure.
    return 10 * GiB;
}

// https://storage.spec.whatwg.org/#obtain-a-local-storage-shelf
GC::Ptr<StorageShelf> obtain_a_local_storage_shelf(HTML::EnvironmentSettingsObject& settings)
{
    // To obtain a local storage shelf, given an environment settings object environment, return the result of running
    // obtain a storage shelf with the user agent’s storage shed, environment, and "local".

    // FIXME: This should be implemented in a way that works for Workers.
    auto& window = HTML::relevant_window(settings.realm().global_object());

    auto key = obtain_a_storage_key(settings);
    if (!key.has_value())
        return {};

    // AD-HOC: The user agent's storage shed is kept by the browser process, in a StorageJar. This shelf is a transient
    //         helper for computing estimate()'s usage and quota — so a standalone shelf is functionally equivalent to
    //         the spec's requirement to obtain one from a shed.
    return StorageShelf::create(window.page(), settings.id, *key, StorageType::Local);
}

}
