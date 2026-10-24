/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageBottle);
GC_DEFINE_ALLOCATOR(StorageBucket);

void StorageBucket::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_bottle_map)
        visitor.visit(entry);
}

StorageBucket::StorageBucket(GC::Ref<Page> page, HTML::EnvironmentId environment_id, StorageKey const& key, StorageType type)
{
    // 1. Let bucket be null.
    // 2. If type is "local", then set bucket to a new local storage bucket.
    // 3. Otherwise:
    //     1. Assert: type is "session".
    //     2. Set bucket to a new session storage bucket.

    // 4. For each endpoint of registered storage endpoints whose types contain type, set bucket’s bottle map[endpoint’s identifier] to a new storage bottle whose quota is endpoint’s quota.
    for (auto const& endpoint : StorageEndpoint::registered_endpoints()) {
        if (endpoint.type == type)
            m_bottle_map[to_underlying(endpoint.identifier)] = StorageBottle::create(page, endpoint, environment_id, key);
    }

    // 5. Return bucket.
}

// https://storage.spec.whatwg.org/#obtain-a-storage-bottle-map
GC::Ptr<StorageBottle> obtain_a_storage_bottle_map(StorageType type, HTML::EnvironmentSettingsObject& environment, StorageEndpointType endpoint_type)
{
    // 1. Let shed be null.
    // 2. If type is "local", then set shed to the user agent’s storage shed.
    // 3. Otherwise:
    //     1. Assert: type is "session".
    //     2. Set shed to environment’s global object’s associated Document’s node navigable’s traversable navigable’s storage shed.
    // NB: Both sheds are kept by the browser process, in a StorageJar, which the bottle returned here mirrors. The
    //     bottle names environment, from which the browser process obtains the storage key itself.

    // 4. Let shelf be the result of running obtain a storage shelf, with shed, environment, and type.
    // 5. If shelf is failure, then return failure.
    auto key = obtain_a_storage_key(environment);
    if (!key.has_value())
        return {};

    // 6. Let bucket be shelf’s bucket map["default"].
    // 7. Let bottle be bucket’s bottle map[identifier].
    // NB: The bucket's bottle map holds a bottle for each registered storage endpoint whose types contain type, with
    //     that endpoint's quota.
    auto endpoint = StorageEndpoint::registered_endpoints().first_matching([&](auto const& endpoint) { return endpoint.identifier == endpoint_type; });
    VERIFY(endpoint.has_value() && endpoint->type == type);

    // 8. Let proxyMap be a new storage proxy map whose backing map is bottle’s map.
    // 9. Append proxyMap to bottle’s proxy map reference set.
    // 10. Return proxyMap.
    auto& page = HTML::relevant_window(environment.global_object()).page();
    return StorageBottle::create(page, *endpoint, environment.id, *key);
}

// https://storage.spec.whatwg.org/#obtain-a-local-storage-bottle-map
GC::Ptr<StorageBottle> obtain_a_local_storage_bottle_map(HTML::EnvironmentSettingsObject& environment, StorageEndpointType identifier)
{
    // To obtain a local storage bottle map, given an environment settings object environment and storage identifier identifier,
    // return the result of running obtain a storage bottle map with "local", environment, and identifier.
    return obtain_a_storage_bottle_map(StorageType::Local, environment, identifier);
}

// https://storage.spec.whatwg.org/#obtain-a-session-storage-bottle-map
GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject& environment, StorageEndpointType identifier)
{
    // To obtain a session storage bottle map, given an environment settings object environment and storage identifier identifier,
    // return the result of running obtain a storage bottle map with "session", environment, and identifier.
    return obtain_a_storage_bottle_map(StorageType::Session, environment, identifier);
}

GC::Ref<StorageBottle> StorageBottle::create(GC::Ref<Page> page, StorageEndpoint const& endpoint, HTML::EnvironmentId environment_id, StorageKey const& key)
{
    return GC::Heap::the().allocate<StorageBottle>(page, endpoint.identifier, move(environment_id), key, endpoint.quota);
}

StorageBottle::StorageBottle(GC::Ref<Page> page, StorageEndpointType endpoint_type, HTML::EnvironmentId environment_id, StorageKey const& key, Optional<u64> quota)
    : m_page(page)
    , m_environment_id(move(environment_id))
    , m_quota(quota)
    // Session storage is partitioned by the traversable, and a page's documents all belong to one traversable, so
    // a session bottle's mirror is found by its page as well as its storage key.
    , m_mirror(StorageAreaMirror::obtain(endpoint_type, endpoint_type == StorageEndpointType::SessionStorage ? Optional<PageId> { page->client().id() } : Optional<PageId> {}, key, quota))
{
    // A change another process made calls for the storage events the spec's broadcast fires, on every Storage object
    // here that shares the map. The mirror is shared, so the first bottle for it hooks this up for all of them.
    if (!m_mirror->on_remote_change) {
        m_mirror->on_remote_change = [mirror = m_mirror->make_weak_ptr()](Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url) {
            if (!mirror)
                return;
            auto type = mirror->endpoint_type() == StorageEndpointType::SessionStorage ? HTML::Storage::Type::Session : HTML::Storage::Type::Local;
            HTML::Storage::dispatch_storage_events_for_remote_change(type, mirror->storage_key(), mirror->session_page(), key, old_value, new_value, url, nullptr);
        };
    }
}

void StorageBottle::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

StorageAreaMirror::Requester StorageBottle::requester() const
{
    return { m_page->client().id(), m_environment_id };
}

void StorageBottle::connect() const
{
    m_mirror->connect(requester());
}

size_t StorageBottle::size() const
{
    return m_mirror->size(requester());
}

Vector<Utf16String> StorageBottle::keys() const
{
    return m_mirror->keys(requester());
}

Optional<Utf16String> StorageBottle::get(Utf16View key) const
{
    return m_mirror->get(requester(), key);
}

StorageSetResult StorageBottle::set(Utf16View key, Utf16View value, String const& document_url)
{
    return m_mirror->set(requester(), key, value, document_url);
}

void StorageBottle::clear(String const& document_url)
{
    m_mirror->clear(requester(), document_url);
}

void StorageBottle::remove(Utf16View key, String const& document_url)
{
    (void)m_mirror->remove(requester(), key, document_url);
}

}
