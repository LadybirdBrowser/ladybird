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
            m_bottle_map[to_underlying(endpoint.identifier)] = StorageBottle::create(page, type, key, endpoint.quota);
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
        shed = &HTML::relevant_window(environment.global_object()).associated_document().navigable()->traversable_navigable()->storage_shed();
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

GC::Ref<StorageBottle> StorageBottle::create(GC::Ref<Page> page, StorageType type, StorageKey key, Optional<u64> quota)
{
    if (type == StorageType::Local)
        return LocalStorageBottle::create(page, key, quota);
    return SessionStorageBottle::create(page, key, quota);
}

static HashMap<String, WeakPtr<CachedStorageMap>>& storage_map_registry(StorageEndpointType endpoint_type)
{
    // Weak — so a map lives exactly as long as some bottle addresses it. Otherwise, a registry owning the maps would
    // retain every map this process ever primed — for every traversable it ever had.
    using Registry = Array<HashMap<String, WeakPtr<CachedStorageMap>>, to_underlying(StorageEndpointType::Count)>;
    // Outlives the process, rather than running a destructor at exit.
    static Registry* registries = new Registry;
    return (*registries)[to_underlying(endpoint_type)];
}

// Bytes one string contributes toward a storage key's quota. The owning process measures the same way — so a write this
// process accepts is a write that process also accepts.
static u64 storage_quota_size(Utf16String const& string)
{
    auto utf8_string = string.to_utf8();
    return utf8_string.bytes().size();
}

void invalidate_cached_storage_maps(StorageEndpointType endpoint_type, String const& storage_key)
{
    for (auto& entry : storage_map_registry(endpoint_type)) {
        // A session storage cache key carries the page it was partitioned under ahead of the storage key — so, match
        // either the whole key or that trailing component.
        auto const& cache_key = entry.key;
        if (cache_key != storage_key && !cache_key.ends_with_bytes(MUST(String::formatted("/{}", storage_key))))
            continue;
        auto map = entry.value.strong_ref();
        if (!map)
            continue;
        map->entries.clear();
        map->quota_used = 0;
        map->primed = false;
    }
}

static u64 next_session_storage_cache_id()
{
    static u64 last_id = 0;
    return ++last_id;
}

String storage_cache_key(StorageEndpointType endpoint_type, String const& storage_key)
{
    // Session storage has exactly one bottle per traversable and storage key: The shed hands out one shelf per storage
    // key, and one bottle per endpoint within it. So a session bottle is not one of several addressing a map — it's the
    // only one, and its cache is its own. That also keeps this off the page's top level traversable — which isn't set
    // yet while an auxiliary page's session storage is being cloned.
    if (endpoint_type == StorageEndpointType::SessionStorage)
        return MUST(String::formatted("{}/{}", next_session_storage_cache_id(), storage_key));

    // Local storage bottles are made one per document, so every bottle for a storage key has to find the same map.
    return storage_key;
}

NonnullRefPtr<CachedStorageMap> cached_storage_map(StorageEndpointType endpoint_type, String const& cache_key)
{
    auto& registry = storage_map_registry(endpoint_type);
    if (auto existing = registry.find(cache_key); existing != registry.end()) {
        if (auto live = existing->value.strong_ref())
            return live.release_nonnull();
    }

    // A weak reference dies with the last bottle addressing it — but its key doesn't. A session-storage key names one
    // bottle, and is never asked for again — so without this, every page ever closed would leave one behind. Sweep them
    // when a new map is made — which is the only moment this registry grows.
    registry.remove_all_matching([](auto const&, auto const& map) { return !map.strong_ref(); });

    auto map = adopt_ref(*new CachedStorageMap);
    registry.set(cache_key, map->make_weak_ptr());
    return map;
}

void LocalStorageBottle::ensure_primed() const
{
    if (m_cache->primed)
        return;

    Vector<Utf16String> keys;
    Vector<Utf16String> values;
    m_page->client().page_did_request_storage_entries(m_endpoint_type, m_storage_key.to_string(), keys, values);

    // The owner sends one value per key. Taking the shorter of the two would prime a map that's quietly missing
    // entries — and nothing downstream could tell that from an empty bottle.
    VERIFY(keys.size() == values.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        m_cache->quota_used += storage_quota_size(keys[i]) + storage_quota_size(values[i]);
        m_cache->entries.set(keys[i], values[i]);
    }
    m_cache->primed = true;
}

size_t LocalStorageBottle::size() const
{
    ensure_primed();
    return m_cache->entries.size();
}

Vector<Utf16String> LocalStorageBottle::keys() const
{
    ensure_primed();
    Vector<Utf16String> keys;
    keys.ensure_capacity(m_cache->entries.size());
    for (auto const& entry : m_cache->entries)
        keys.unchecked_append(entry.key);
    return keys;
}

Optional<Utf16String> LocalStorageBottle::get(Utf16View key) const
{
    ensure_primed();
    auto entry = m_cache->entries.find(Utf16String::from_utf16(key));
    if (entry == m_cache->entries.end())
        return {};
    return entry->value;
}

StorageSetResult LocalStorageBottle::set(Utf16View key, Utf16View value)
{
    // OPTIMIZATION: An item too large to ever fit is rejected before priming — so storing one enormous string into an
    // origin this process hasn't read yet costs no round trip. Quota is measured in UTF-8 bytes, and a UTF-16 code unit
    // never encodes to fewer than one of those. So, the code unit count is a lower bound — and exceeding the quota with
    // it means exceeding the quota outright.
    if (m_quota.has_value() && key.length_in_code_units() + value.length_in_code_units() > *m_quota)
        return WebView::StorageOperationError::QuotaExceededError;

    ensure_primed();
    auto owned_key = Utf16String::from_utf16(key);
    auto owned_value = Utf16String::from_utf16(value);

    Optional<Utf16String> old_value;
    u64 replaced_size = 0;
    if (auto existing = m_cache->entries.find(owned_key); existing != m_cache->entries.end()) {
        old_value = existing->value;
        replaced_size = storage_quota_size(owned_key) + storage_quota_size(existing->value);
    }

    // INTEROP: This answers quota from the cache and sends the write without waiting — so a refusal the owner makes
    //          afterwards reaches no one. Chromium decides the same way — from its renderer-local StorageAreaMap in
    //          CachedStorageArea::SetItem() — and discards what the browser process answers. Waiting for that answer
    //          would put a sync round trip back on every write — which is exactly the cost this cache exists to remove.
    //          The owner still refuses a write that doesn't fit – so nothing over quota is persisted. And it tells this
    //          process to drop the map — so what gets read afterwards is right.
    auto new_size = storage_quota_size(owned_key) + storage_quota_size(owned_value);
    auto used_after = m_cache->quota_used - replaced_size + new_size;
    if (m_quota.has_value() && used_after > *m_quota)
        return WebView::StorageOperationError::QuotaExceededError;

    m_cache->quota_used = used_after;
    m_cache->entries.set(owned_key, owned_value);
    m_page->client().page_did_set_storage_item(m_endpoint_type, m_storage_key.to_string(), owned_key, owned_value);
    return old_value;
}

void LocalStorageBottle::clear()
{
    m_page->client().page_did_clear_storage(m_endpoint_type, m_storage_key.to_string());
    m_cache->entries.clear();
    m_cache->quota_used = 0;
    m_cache->primed = true;
}

void LocalStorageBottle::remove(Utf16View key)
{
    ensure_primed();
    auto owned_key = Utf16String::from_utf16(key);
    if (auto existing = m_cache->entries.find(owned_key); existing != m_cache->entries.end())
        m_cache->quota_used -= storage_quota_size(owned_key) + storage_quota_size(existing->value);
    m_cache->entries.remove(owned_key);
    m_page->client().page_did_remove_storage_item(m_endpoint_type, m_storage_key.to_string(), owned_key);
}

void LocalStorageBottle::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

void SessionStorageBottle::ensure_primed() const
{
    if (m_cache->primed)
        return;

    Vector<Utf16String> keys;
    Vector<Utf16String> values;
    m_page->client().page_did_request_storage_entries(m_endpoint_type, m_storage_key.to_string(), keys, values);

    // The owner sends one value per key. Taking the shorter of the two would prime a map that is
    // quietly missing entries, and nothing downstream could tell that from an empty bottle.
    VERIFY(keys.size() == values.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        m_cache->quota_used += storage_quota_size(keys[i]) + storage_quota_size(values[i]);
        m_cache->entries.set(keys[i], values[i]);
    }
    m_cache->primed = true;
}

size_t SessionStorageBottle::size() const
{
    ensure_primed();
    return m_cache->entries.size();
}

Vector<Utf16String> SessionStorageBottle::keys() const
{
    ensure_primed();
    Vector<Utf16String> keys;
    keys.ensure_capacity(m_cache->entries.size());
    for (auto const& entry : m_cache->entries)
        keys.unchecked_append(entry.key);
    return keys;
}

Optional<Utf16String> SessionStorageBottle::get(Utf16View key) const
{
    ensure_primed();
    auto entry = m_cache->entries.find(Utf16String::from_utf16(key));
    if (entry == m_cache->entries.end())
        return {};
    return entry->value;
}

StorageSetResult SessionStorageBottle::set(Utf16View key, Utf16View value)
{
    // OPTIMIZATION: An item too large to ever fit is rejected before priming — so storing one enormous string into an
    // origin this process hasn't read yet costs no round trip. Quota is measured in UTF-8 bytes, and a UTF-16 code unit
    // never encodes to fewer than one of those. So, the code unit count is a lower bound — and exceeding the quota with
    // it means exceeding the quota outright.
    if (m_quota.has_value() && key.length_in_code_units() + value.length_in_code_units() > *m_quota)
        return WebView::StorageOperationError::QuotaExceededError;

    ensure_primed();
    auto owned_key = Utf16String::from_utf16(key);
    auto owned_value = Utf16String::from_utf16(value);

    Optional<Utf16String> old_value;
    u64 replaced_size = 0;
    if (auto existing = m_cache->entries.find(owned_key); existing != m_cache->entries.end()) {
        old_value = existing->value;
        replaced_size = storage_quota_size(owned_key) + storage_quota_size(existing->value);
    }

    // INTEROP: This answers quota from the cache and sends the write without waiting — so a refusal the owner makes
    //          afterwards reaches no one. Chromium decides the same way — from its renderer-local StorageAreaMap in
    //          CachedStorageArea::SetItem() — and discards what the browser process answers. Waiting for that answer
    //          would put a sync round trip back on every write — which is exactly the cost this cache exists to remove.
    //          The owner still refuses a write that doesn't fit – so nothing over quota is persisted. And it tells this
    //          process to drop the map — so what gets read afterwards is right.
    auto new_size = storage_quota_size(owned_key) + storage_quota_size(owned_value);
    auto used_after = m_cache->quota_used - replaced_size + new_size;
    if (m_quota.has_value() && used_after > *m_quota)
        return WebView::StorageOperationError::QuotaExceededError;

    m_cache->quota_used = used_after;
    m_cache->entries.set(owned_key, owned_value);
    m_page->client().page_did_set_storage_item(m_endpoint_type, m_storage_key.to_string(), owned_key, owned_value);
    return old_value;
}

void SessionStorageBottle::clear()
{
    m_page->client().page_did_clear_storage(m_endpoint_type, m_storage_key.to_string());
    m_cache->entries.clear();
    m_cache->quota_used = 0;
    m_cache->primed = true;
}

void SessionStorageBottle::remove(Utf16View key)
{
    ensure_primed();
    auto owned_key = Utf16String::from_utf16(key);
    if (auto existing = m_cache->entries.find(owned_key); existing != m_cache->entries.end())
        m_cache->quota_used -= storage_quota_size(owned_key) + storage_quota_size(existing->value);
    m_cache->entries.remove(owned_key);
    m_page->client().page_did_remove_storage_item(m_endpoint_type, m_storage_key.to_string(), owned_key);
}

void SessionStorageBottle::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

void SessionStorageBottle::copy_map_from(SessionStorageBottle const& other)
{
    clear();
    for (auto const& key : other.keys()) {
        auto value = other.get(key);
        VERIFY(value.has_value());
        auto result = set(key, *value);
        VERIFY(result.has<Optional<Utf16String>>());
    }
}

}
