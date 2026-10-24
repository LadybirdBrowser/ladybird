/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/StorageAPI/StorageAreaMirror.h>

namespace Web::StorageAPI {

static StorageAreaMirrorClient* s_client = nullptr;

void set_storage_area_mirror_client(StorageAreaMirrorClient* client)
{
    s_client = client;
}

// The mirrors this process holds, by the key a bottle finds them with and by the area id the owner's messages name.
// Weak, so a mirror lives exactly as long as some bottle addresses it; each mirror removes itself when it dies.
struct MirrorRegistry {
    AK_ALLOC_WITH_KMALLOC;

    HashMap<String, WeakPtr<StorageAreaMirror>> by_key;
    HashMap<StorageAreaId, WeakPtr<StorageAreaMirror>> by_id;
};

static MirrorRegistry& registry()
{
    // Outlives the process, rather than running a destructor at exit.
    static auto* registry = new MirrorRegistry;
    return *registry;
}

static String registry_key_for(StorageEndpointType endpoint_type, Optional<PageId> session_page, StorageKey const& storage_key)
{
    if (session_page.has_value())
        return MUST(String::formatted("{}/{}/{}", to_underlying(endpoint_type), *session_page, storage_key.to_string()));
    return MUST(String::formatted("{}//{}", to_underlying(endpoint_type), storage_key.to_string()));
}

NonnullRefPtr<StorageAreaMirror> StorageAreaMirror::obtain(StorageEndpointType endpoint_type, Optional<PageId> session_page, StorageKey const& storage_key, Optional<u64> quota)
{
    auto key = registry_key_for(endpoint_type, session_page, storage_key);
    if (auto existing = registry().by_key.find(key); existing != registry().by_key.end()) {
        if (auto live = existing->value.strong_ref())
            return live.release_nonnull();
    }

    auto mirror = adopt_ref(*new StorageAreaMirror(endpoint_type, session_page, storage_key, quota, key));
    registry().by_key.set(key, mirror->make_weak_ptr());
    return mirror;
}

StorageAreaMirror::StorageAreaMirror(StorageEndpointType endpoint_type, Optional<PageId> session_page, StorageKey storage_key, Optional<u64> quota, String registry_key)
    : m_endpoint_type(endpoint_type)
    , m_session_page(session_page)
    , m_storage_key(move(storage_key))
    , m_quota(quota)
    , m_registry_key(move(registry_key))
{
}

StorageAreaMirror::~StorageAreaMirror()
{
    registry().by_key.remove(m_registry_key);
    if (m_id.has_value()) {
        registry().by_id.remove(*m_id);
        if (s_client)
            s_client->disconnect_from_storage_area(*m_id);
    }
}

static String requester_key(StorageAreaMirror::Requester const& requester)
{
    return MUST(String::formatted("{}/{}", requester.page_id, requester.environment_id));
}

bool StorageAreaMirror::ensure_primed(Requester const& requester)
{
    if (m_primed)
        return true;
    if (!s_client)
        return false;

    auto key = requester_key(requester);
    if (m_failed_requesters.contains(key))
        return false;

    // One round trip loads the map and registers this process for every change after it, so nothing is missed or
    // seen twice.
    auto connection = s_client->connect_to_storage_area(requester.page_id, m_endpoint_type, requester.environment_id);
    if (!connection.id.has_value()) {
        m_failed_requesters.set(move(key));
        return false;
    }

    // The owner sends one value per key. Taking the shorter of the two would prime a map that's quietly missing
    // entries, and nothing downstream could tell that from an empty bottle.
    VERIFY(connection.keys.size() == connection.values.size());

    m_id = connection.id;
    registry().by_id.set(*m_id, make_weak_ptr());
    for (size_t i = 0; i < connection.keys.size(); ++i) {
        auto quota_size = storage_quota_size(connection.keys[i]) + storage_quota_size(connection.values[i]);
        m_entries.set(move(connection.keys[i]), Entry { move(connection.values[i]), quota_size });
    }
    m_quota_used = connection.usage;
    m_primed = true;
    m_failed_requesters.clear();
    return true;
}

size_t StorageAreaMirror::size(Requester const& requester)
{
    if (!ensure_primed(requester))
        return 0;
    return m_entries.size();
}

Vector<Utf16String> StorageAreaMirror::keys(Requester const& requester)
{
    if (!ensure_primed(requester))
        return {};
    Vector<Utf16String> keys;
    keys.ensure_capacity(m_entries.size());
    for (auto const& entry : m_entries)
        keys.unchecked_append(entry.key);
    return keys;
}

Optional<Utf16String> StorageAreaMirror::get(Requester const& requester, Utf16View key)
{
    if (!ensure_primed(requester))
        return {};
    auto entry = m_entries.find(Utf16String::from_utf16(key));
    if (entry == m_entries.end())
        return {};
    return entry->value.value;
}

WebView::StorageSetResult StorageAreaMirror::set(Requester const& requester, Utf16View key, Utf16View value, String const& document_url)
{
    // OPTIMIZATION: An item too large to ever fit is refused before loading the map, so storing one enormous string
    // into a bottle this process hasn't read yet costs no round trip. Quota is measured in UTF-8 bytes, and a UTF-16
    // code unit never encodes to fewer than one of those, so the code unit count is a lower bound.
    if (m_quota.has_value() && key.length_in_code_units() + value.length_in_code_units() > *m_quota)
        return WebView::StorageOperationError::QuotaExceededError;

    if (!ensure_primed(requester))
        return WebView::StorageOperationError::QuotaExceededError;

    auto owned_key = Utf16String::from_utf16(key);
    auto owned_value = Utf16String::from_utf16(value);

    Optional<Utf16String> old_value;
    u64 replaced_size = 0;
    if (auto existing = m_entries.find(owned_key); existing != m_entries.end()) {
        old_value = existing->value.value;
        replaced_size = existing->value.quota_size;
    }

    // The same rule the owner applies, so a write accepted here is one the owner accepts too — unless another
    // process filled the bottle in between, which the owner then refuses and this mirror hears of.
    auto new_size = storage_quota_size(key) + storage_quota_size(value);
    if (m_quota.has_value() && m_quota_used - replaced_size + new_size > *m_quota)
        return WebView::StorageOperationError::QuotaExceededError;

    apply_set(owned_key, owned_value, new_size);
    push_pending_write(owned_key, owned_value);
    s_client->set_storage_item(*m_id, owned_key, owned_value, document_url);
    return old_value;
}

Optional<Utf16String> StorageAreaMirror::remove(Requester const& requester, Utf16View key, String const& document_url)
{
    if (!ensure_primed(requester))
        return {};

    auto owned_key = Utf16String::from_utf16(key);
    auto existing = m_entries.find(owned_key);
    if (existing == m_entries.end())
        return {};

    auto old_value = existing->value.value;
    apply_remove(owned_key);
    push_pending_write(owned_key, {});
    s_client->remove_storage_item(*m_id, owned_key, document_url);
    return old_value;
}

bool StorageAreaMirror::clear(Requester const& requester, String const& document_url)
{
    if (!ensure_primed(requester) || m_entries.is_empty())
        return false;

    apply_clear();
    push_pending_write({}, {});
    s_client->clear_storage_area(*m_id, document_url);
    return true;
}

void StorageAreaMirror::apply_set(Utf16String const& key, Utf16String const& value, u64 quota_size)
{
    if (auto existing = m_entries.find(key); existing != m_entries.end())
        m_quota_used -= existing->value.quota_size;
    m_quota_used += quota_size;
    m_entries.set(key, Entry { value, quota_size });
}

void StorageAreaMirror::apply_remove(Utf16String const& key)
{
    if (auto existing = m_entries.find(key); existing != m_entries.end()) {
        m_quota_used -= existing->value.quota_size;
        m_entries.remove(existing);
    }
}

void StorageAreaMirror::apply_clear()
{
    m_entries.clear();
    m_quota_used = 0;
}

void StorageAreaMirror::push_pending_write(Optional<Utf16String> key, Optional<Utf16String> new_value)
{
    if (key.has_value())
        ++m_pending_count_by_key.ensure(*key);
    else
        ++m_pending_clear_count;
    m_pending_writes.append({ move(key), move(new_value) });
}

StorageAreaMirror::PendingWrite StorageAreaMirror::pop_pending_write()
{
    // The owner answers every write, in the order they were sent.
    VERIFY(m_first_pending_write < m_pending_writes.size());
    auto write = move(m_pending_writes[m_first_pending_write++]);
    if (m_first_pending_write == m_pending_writes.size()) {
        m_pending_writes.clear();
        m_first_pending_write = 0;
    }
    if (write.key.has_value()) {
        auto count = m_pending_count_by_key.get(*write.key);
        VERIFY(count.has_value() && *count > 0);
        if (*count == 1)
            m_pending_count_by_key.remove(*write.key);
        else
            m_pending_count_by_key.set(*write.key, *count - 1);
    } else {
        VERIFY(m_pending_clear_count > 0);
        --m_pending_clear_count;
    }
    return write;
}

// The owner hasn't applied these yet, so whatever state it just described predates them.
void StorageAreaMirror::reapply_pending_writes()
{
    for (auto const& write : m_pending_writes.span().slice(m_first_pending_write)) {
        if (!write.key.has_value())
            apply_clear();
        else if (write.new_value.has_value())
            apply_set(*write.key, *write.new_value, storage_quota_size(*write.key) + storage_quota_size(*write.new_value));
        else
            apply_remove(*write.key);
    }
}

void StorageAreaMirror::reset_to(Vector<Utf16String> keys, Vector<Utf16String> values)
{
    VERIFY(keys.size() == values.size());

    auto old_entries = move(m_entries);
    m_entries.clear();
    m_quota_used = 0;
    for (size_t i = 0; i < keys.size(); ++i)
        apply_set(keys[i], values[i], storage_quota_size(keys[i]) + storage_quota_size(values[i]));
    reapply_pending_writes();

    if (!on_remote_change)
        return;

    // AD-HOC: The spec's broadcast fires an event for each change a script makes, and never for a write refused after
    //         the fact; it has no such thing. Whatever differs from before is a change this process's windows didn't
    //         see being made, so each gets an event for it — the window whose write was refused included.
    auto url = m_storage_key.to_string();
    for (auto const& entry : old_entries) {
        Optional<Utf16String> now;
        if (auto it = m_entries.find(entry.key); it != m_entries.end())
            now = it->value.value;
        if (now != entry.value.value)
            on_remote_change(entry.key, entry.value.value, now, url);
    }
    for (auto const& entry : m_entries) {
        if (!old_entries.contains(entry.key))
            on_remote_change(entry.key, {}, entry.value.value, url);
    }
}

// A change another process made, as the owner serialized it. One that reached the owner before a write this process
// still has pending predates that write, so the mirror already shows the later state and leaves it be. While a clear
// is pending, that goes for every change.
void StorageAreaMirror::apply_remote_change(Optional<Utf16String> const& key, Optional<Utf16String> const& new_value)
{
    if (m_pending_clear_count > 0)
        return;

    if (!key.has_value()) {
        // The owner emptied the bottle before applying this process's pending writes, so those come back on top.
        apply_clear();
        reapply_pending_writes();
        return;
    }

    if (m_pending_count_by_key.contains(*key))
        return;

    if (new_value.has_value())
        apply_set(*key, *new_value, storage_quota_size(*key) + storage_quota_size(*new_value));
    else
        apply_remove(*key);
}

static RefPtr<StorageAreaMirror> mirror_for(StorageAreaId id)
{
    auto entry = registry().by_id.find(id);
    if (entry == registry().by_id.end())
        return {};
    return entry->value.strong_ref();
}

void StorageAreaMirror::did_change(StorageAreaId id, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url)
{
    auto mirror = mirror_for(id);
    if (!mirror)
        return;

    mirror->apply_remote_change(key, new_value);
    if (mirror->on_remote_change)
        mirror->on_remote_change(key, old_value, new_value, url);
}

void StorageAreaMirror::did_acknowledge_write(StorageAreaId id)
{
    auto mirror = mirror_for(id);
    if (!mirror)
        return;
    mirror->pop_pending_write();
}

void StorageAreaMirror::did_refuse_write(StorageAreaId id, Vector<Utf16String> keys, Vector<Utf16String> values)
{
    auto mirror = mirror_for(id);
    if (!mirror)
        return;

    // Only a set can be refused: it was applied here on the strength of a quota check another process's write
    // overtook. The owner's map puts this one back in agreement.
    auto refused = mirror->pop_pending_write();
    VERIFY(refused.key.has_value() && refused.new_value.has_value());
    mirror->reset_to(move(keys), move(values));
}

void StorageAreaMirror::did_reset(StorageAreaId id, Vector<Utf16String> keys, Vector<Utf16String> values)
{
    auto mirror = mirror_for(id);
    if (!mirror)
        return;
    mirror->reset_to(move(keys), move(values));
}

}
