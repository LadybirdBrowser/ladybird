/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/StorageAPI/StorageShed.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(LocalStorageBottle);
GC_DEFINE_ALLOCATOR(SessionStorageBottle);
GC_DEFINE_ALLOCATOR(StorageBucket);

static size_t storage_quota_size(Utf16View string)
{
    auto utf8_string = MUST(string.to_utf8());
    return utf8_string.bytes().size();
}

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
            m_bottle_map[to_underlying(endpoint.identifier)] = StorageBottle::create(heap(), page, type, endpoint.identifier, key, endpoint.quota);
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

GC::Ref<StorageBottle> StorageBottle::create(GC::Heap& heap, GC::Ref<Page> page, StorageType type, StorageEndpointType endpoint_type, StorageKey key, Optional<u64> quota)
{
    if (type == StorageType::Local)
        return LocalStorageBottle::create(heap, page, endpoint_type, key, quota);
    return SessionStorageBottle::create(heap, quota);
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

size_t SessionStorageBottle::size() const
{
    return m_map.size();
}

Vector<Utf16String> SessionStorageBottle::keys() const
{
    return m_map.keys();
}

Optional<Utf16String> SessionStorageBottle::get(Utf16View key) const
{
    if (auto entry = m_map.get(key); entry.has_value())
        return entry->value;
    return OptionalNone {};
}

StorageSetResult SessionStorageBottle::set(Utf16View key, Utf16View value)
{
    auto old_value = get(key);

    auto new_size = storage_quota_size(key) + storage_quota_size(value);

    if (m_quota.has_value()) {
        size_t current_size = 0;
        for (auto const& [existing_key, existing_entry] : m_map) {
            if (existing_key.utf16_view() != key)
                current_size += existing_entry.quota_size;
        }
        if (current_size + new_size > m_quota.value())
            return WebView::StorageOperationError::QuotaExceededError;
    }

    m_map.set(Utf16String::from_utf16(key), { Utf16String::from_utf16(value), new_size });
    return old_value;
}

void SessionStorageBottle::clear()
{
    m_map.clear();
}

void SessionStorageBottle::remove(Utf16View key)
{
    m_map.remove(key);
}

void SessionStorageBottle::copy_map_from(SessionStorageBottle const& other)
{
    m_map = other.m_map;
}

}
