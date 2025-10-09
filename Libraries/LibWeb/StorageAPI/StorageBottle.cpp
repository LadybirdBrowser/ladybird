/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/StorageAPI/StorageShed.h>

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

StorageBucket::StorageBucket(GC::Ref<Page> page, StorageKey const& key, StorageType type)
{
    // 1. Let bucket be null.
    // 2. If type is "local", then set bucket to a new local storage bucket.
    // 3. Otherwise:
    //     1. Assert: type is "session".
    //     2. Set bucket to a new session storage bucket.

    // 4. For each endpoint of registered storage endpoints whose types contain type, set bucket’s bottle map[endpoint’s identifier] to a new storage bottle whose quota is endpoint’s quota.
    for (auto const& endpoint : StorageEndpoint::registered_endpoints()) {
        if (endpoint.type == type)
            m_bottle_map[to_underlying(endpoint.identifier)] = StorageBottle::create(heap(), page, type, key, endpoint.quota);
    }

    // 5. Return bucket.
}

// https://storage.spec.whatwg.org/#obtain-a-storage-bottle-map
GC::Ptr<StorageBottle> obtain_a_storage_bottle_map(StorageType type, HTML::EnvironmentSettingsObject& environment, StorageEndpointType endpoint_type)
{
    // 1. Let shed be null.
    GC::Ptr<StorageShed> shed;

    // 2. If type is "local", then set shed to the user agent’s storage shed.
    if (type == StorageType::Local) {
        // NOTE: Bottle for local storage is constructed directly, bypassing this function, because
        //       in that case StorageJar located on browser process side is used as a shed.
        VERIFY_NOT_REACHED();
    }
    // 3. Otherwise:
    else {
        // 1. Assert: type is "session".
        VERIFY(type == StorageType::Session);

        // 2. Set shed to environment’s global object’s associated Document’s node navigable’s traversable navigable’s storage shed.
        shed = &as<HTML::Window>(environment.global_object()).associated_document().navigable()->traversable_navigable()->storage_shed();
    }

    // 4. Let shelf be the result of running obtain a storage shelf, with shed, environment, and type.
    VERIFY(shed);
    auto shelf = shed->obtain_a_storage_shelf(environment, type);

    // 5. If shelf is failure, then return failure.
    if (!shelf)
        return {};

    // 6. Let bucket be shelf’s bucket map["default"].
    auto bucket = shelf->bucket_map().get("default"sv).value();

    // 7. Let bottle be bucket’s bottle map[identifier].
    auto bottle = bucket->bottle_map()[to_underlying(endpoint_type)];

    // 8. Let proxyMap be a new storage proxy map whose backing map is bottle’s map.
    // 9. Append proxyMap to bottle’s proxy map reference set.
    // 10. Return proxyMap.
    return bottle->proxy();
}

// https://storage.spec.whatwg.org/#obtain-a-session-storage-bottle-map
GC::Ptr<StorageBottle> obtain_a_session_storage_bottle_map(HTML::EnvironmentSettingsObject& environment, StorageEndpointType identifier)
{
    // To obtain a session storage bottle map, given an environment settings object environment and storage identifier identifier,
    // return the result of running obtain a storage bottle map with "session", environment, and identifier.
    return obtain_a_storage_bottle_map(StorageType::Session, environment, identifier);
}

GC::Ref<StorageBottle> StorageBottle::create(GC::Heap& heap, GC::Ref<Page> page, StorageType type, StorageKey const& key, Optional<u64> quota)
{
    if (type == StorageType::Local)
        return LocalStorageBottle::create(heap, page, move(key), quota);
    return SessionStorageBottle::create(heap, quota);
}

void LocalStorageBottle::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

size_t LocalStorageBottle::size() const
{
    return m_page->client().page_did_request_storage_keys(Web::StorageAPI::StorageEndpointType::LocalStorage, m_storage_key.to_string()).size();
}

Vector<String> LocalStorageBottle::keys() const
{
    return m_page->client().page_did_request_storage_keys(Web::StorageAPI::StorageEndpointType::LocalStorage, m_storage_key.to_string());
}

Optional<String> LocalStorageBottle::get(String const& key) const
{
    return m_page->client().page_did_request_storage_item(Web::StorageAPI::StorageEndpointType::LocalStorage, m_storage_key.to_string(), key);
}

WebView::StorageOperationError LocalStorageBottle::set(String const& key, String const& value)
{
    return m_page->client().page_did_set_storage_item(Web::StorageAPI::StorageEndpointType::LocalStorage, m_storage_key.to_string(), key, value);
}

void LocalStorageBottle::clear()
{
    m_page->client().page_did_clear_storage(Web::StorageAPI::StorageEndpointType::LocalStorage, m_storage_key.to_string());
}

void LocalStorageBottle::remove(String const& key)
{
    m_page->client().page_did_remove_storage_item(Web::StorageAPI::StorageEndpointType::LocalStorage, m_storage_key.to_string(), key);
}

size_t SessionStorageBottle::size() const
{
    return m_map.size();
}

Vector<String> SessionStorageBottle::keys() const
{
    return m_map.keys();
}

Optional<String> SessionStorageBottle::get(String const& key) const
{
    if (auto value = m_map.get(key); value.has_value())
        return value.value();
    return OptionalNone {};
}

WebView::StorageOperationError SessionStorageBottle::set(String const& key, String const& value)
{
    if (m_quota.has_value()) {
        size_t current_size = 0;
        for (auto const& [existing_key, existing_value] : m_map) {
            if (existing_key != key) {
                current_size += existing_key.bytes().size();
                current_size += existing_value.bytes().size();
            }
        }
        size_t new_size = key.bytes().size() + value.bytes().size();
        if (current_size + new_size > m_quota.value())
            return WebView::StorageOperationError::QuotaExceededError;
    }

    m_map.set(key, value);
    return WebView::StorageOperationError::None;
}

void SessionStorageBottle::clear()
{
    m_map.clear();
}

void SessionStorageBottle::remove(String const& key)
{
    m_map.remove(key);
}

}
