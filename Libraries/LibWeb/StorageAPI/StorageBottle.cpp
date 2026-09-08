/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(LocalStorageBottle);
GC_DEFINE_ALLOCATOR(SessionStorageBottle);
GC_DEFINE_ALLOCATOR(StorageBucket);

void StorageBucket::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_bottle_map)
        visitor.visit(entry);
}

StorageBucket::StorageBucket(GC::Ref<Page> page, StorageKey key, StorageType type)
{
    // 1. Let bucket be null.
    // 2. If type is "local", then set bucket to a new local storage bucket.
    // 3. Otherwise:
    //     1. Assert: type is "session".
    //     2. Set bucket to a new session storage bucket.

    // 4. For each endpoint of registered storage endpoints whose types contain type, set bucket’s bottle map[endpoint’s identifier] to a new storage bottle whose quota is endpoint’s quota.
    for (auto const& endpoint : StorageEndpoint::registered_endpoints()) {
        if (endpoint.type == type)
            m_bottle_map[to_underlying(endpoint.identifier)] = StorageBottle::create(page, endpoint, key);
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
    // NB: The user agent’s storage shed and each traversable navigable’s storage shed are kept by the browser process,
    //     in a StorageJar, which the bottles here proxy. Bottles of type "local" bypass this function entirely.
    VERIFY(type == StorageType::Session);
    VERIFY(endpoint_type == StorageEndpointType::SessionStorage);

    // 4. Let shelf be the result of running obtain a storage shelf, with shed, environment, and type.
    // 5. If shelf is failure, then return failure.
    auto key = obtain_a_storage_key(environment);
    if (!key.has_value())
        return {};

    // 6. Let bucket be shelf’s bucket map["default"].
    // 7. Let bottle be bucket’s bottle map[identifier].
    // 8. Let proxyMap be a new storage proxy map whose backing map is bottle’s map.
    // 9. Append proxyMap to bottle’s proxy map reference set.
    // 10. Return proxyMap.
    auto& page = HTML::relevant_window(environment.global_object()).page();
    return SessionStorageBottle::create(page, StorageEndpointType::SessionStorage, key.release_value(), StorageEndpoint::SESSION_STORAGE_QUOTA);
}

// https://storage.spec.whatwg.org/#obtain-a-session-storage-bottle-map
GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject& environment, StorageEndpointType identifier)
{
    // To obtain a session storage bottle map, given an environment settings object environment and storage identifier identifier,
    // return the result of running obtain a storage bottle map with "session", environment, and identifier.
    return obtain_a_storage_bottle_map(StorageType::Session, environment, identifier);
}

GC::Ref<StorageBottle> StorageBottle::create(GC::Ref<Page> page, StorageEndpoint const& endpoint, StorageKey key)
{
    if (endpoint.type == StorageType::Local)
        return LocalStorageBottle::create(page, endpoint.identifier, move(key), endpoint.quota);
    return SessionStorageBottle::create(page, endpoint.identifier, move(key), endpoint.quota);
}

void LocalStorageBottle::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

size_t LocalStorageBottle::size() const
{
    return m_page->client().page_did_request_storage_keys(m_endpoint_type, m_storage_key.to_string()).size();
}

Vector<Utf16String> LocalStorageBottle::keys() const
{
    return m_page->client().page_did_request_storage_keys(m_endpoint_type, m_storage_key.to_string());
}

Optional<Utf16String> LocalStorageBottle::get(Utf16View key) const
{
    return m_page->client().page_did_request_storage_item(m_endpoint_type, m_storage_key.to_string(), Utf16String::from_utf16(key));
}

StorageSetResult LocalStorageBottle::set(Utf16View key, Utf16View value)
{
    return m_page->client().page_did_set_storage_item(m_endpoint_type, m_storage_key.to_string(), Utf16String::from_utf16(key), Utf16String::from_utf16(value));
}

void LocalStorageBottle::clear()
{
    m_page->client().page_did_clear_storage(m_endpoint_type, m_storage_key.to_string());
}

void LocalStorageBottle::remove(Utf16View key)
{
    m_page->client().page_did_remove_storage_item(m_endpoint_type, m_storage_key.to_string(), Utf16String::from_utf16(key));
}

void SessionStorageBottle::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

size_t SessionStorageBottle::size() const
{
    return m_page->client().page_did_request_storage_keys(m_endpoint_type, m_storage_key.to_string()).size();
}

Vector<Utf16String> SessionStorageBottle::keys() const
{
    return m_page->client().page_did_request_storage_keys(m_endpoint_type, m_storage_key.to_string());
}

Optional<Utf16String> SessionStorageBottle::get(Utf16View key) const
{
    return m_page->client().page_did_request_storage_item(m_endpoint_type, m_storage_key.to_string(), Utf16String::from_utf16(key));
}

StorageSetResult SessionStorageBottle::set(Utf16View key, Utf16View value)
{
    return m_page->client().page_did_set_storage_item(m_endpoint_type, m_storage_key.to_string(), Utf16String::from_utf16(key), Utf16String::from_utf16(value));
}

void SessionStorageBottle::clear()
{
    m_page->client().page_did_clear_storage(m_endpoint_type, m_storage_key.to_string());
}

void SessionStorageBottle::remove(Utf16View key)
{
    m_page->client().page_did_remove_storage_item(m_endpoint_type, m_storage_key.to_string(), Utf16String::from_utf16(key));
}

}
