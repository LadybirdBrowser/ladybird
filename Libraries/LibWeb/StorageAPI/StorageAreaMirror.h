/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibWeb/Export.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/StorageAPI/StorageAreaId.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>
#include <LibWebCommon/WebView/StorageSetResult.h>

namespace Web::StorageAPI {

// What the process that owns the storage areas answers when this process connects to one: the area's id and
// everything the bottle holds. No id means the area couldn't be resolved, and the mirror stays detached.
struct StorageAreaConnection {
    Optional<StorageAreaId> id;
    Vector<Utf16String> keys;
    Vector<Utf16String> values;
    u64 usage { 0 };
};

// The process's one link to the storage areas' owner. WebContent connects it to the browser process; a process without
// one (a unit test) leaves every mirror detached.
class WEB_API StorageAreaMirrorClient {
public:
    virtual ~StorageAreaMirrorClient() = default;

    virtual StorageAreaConnection connect_to_storage_area(PageId, StorageEndpointType, HTML::EnvironmentId const&) = 0;
    virtual void disconnect_from_storage_area(StorageAreaId) = 0;
    virtual void set_storage_item(StorageAreaId, Utf16String const& key, Utf16String const& value, String const& document_url) = 0;
    virtual void remove_storage_item(StorageAreaId, Utf16String const& key, String const& document_url) = 0;
    virtual void clear_storage_area(StorageAreaId, String const& document_url) = 0;
};

WEB_API void set_storage_area_mirror_client(StorageAreaMirrorClient*);

// https://storage.spec.whatwg.org/#storage-bottle
// This process's copy of one storage bottle's map. The map itself is kept by the browser process, which makes it
// available across agent cluster boundaries this way: a process loads the map once, with one synchronous round trip
// that also registers it for every later change, and from then on answers reads from the copy, applies writes to the
// copy before sending them, and applies the changes other processes make as the owner relays them. Every storage
// bottle in this process for the same area shares one mirror, so two windows of one origin can't see different maps.
class WEB_API StorageAreaMirror final
    : public RefCounted<StorageAreaMirror>
    , public Weakable<StorageAreaMirror> {
public:
    // The page and environment a bottle asks on behalf of, which the owner resolves to the area. A mirror outlives
    // the document that first asked for it, so every call names its own requester.
    struct Requester {
        PageId page_id;
        HTML::EnvironmentId environment_id;
    };

    // One mirror per storage key for local storage, and per page and storage key for session storage: a page's
    // documents all belong to one traversable, and the traversable is what partitions session storage.
    static NonnullRefPtr<StorageAreaMirror> obtain(StorageEndpointType, Optional<PageId> session_page, StorageKey const&, Optional<u64> quota);
    ~StorageAreaMirror();

    size_t size(Requester const&);
    Vector<Utf16String> keys(Requester const&);
    Optional<Utf16String> get(Requester const&, Utf16View key);
    // The value the key had, or the quota's refusal. The write is applied here and sent on without waiting.
    WebView::StorageSetResult set(Requester const&, Utf16View key, Utf16View value, String const& document_url);
    // The value removed, if the key existed.
    Optional<Utf16String> remove(Requester const&, Utf16View key, String const& document_url);
    // False when there was nothing to clear.
    bool clear(Requester const&, String const& document_url);

    // What the owner relays, found by area id. A change for an area this process no longer mirrors is dropped.
    static void did_change(StorageAreaId, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url);
    static void did_acknowledge_write(StorageAreaId);
    static void did_refuse_write(StorageAreaId, Vector<Utf16String> keys, Vector<Utf16String> values);
    static void did_reset(StorageAreaId, Vector<Utf16String> keys, Vector<Utf16String> values);

    // Every change another party made to the map, as the owner relayed it (or, after a reset, as the diff shows it),
    // for the storage events it calls for. A null key is a clear.
    Function<void(Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url)> on_remote_change;

    StorageEndpointType endpoint_type() const { return m_endpoint_type; }
    StorageKey const& storage_key() const { return m_storage_key; }
    Optional<PageId> session_page() const { return m_session_page; }

    u64 quota_used() const { return m_quota_used; }
    size_t pending_write_count() const { return m_pending_writes.size() - m_first_pending_write; }

private:
    StorageAreaMirror(StorageEndpointType, Optional<PageId> session_page, StorageKey, Optional<u64> quota, String registry_key);

    // A write this process applied and sent, until the owner acknowledges or refuses it. A null key is a clear; a
    // null value is a removal.
    struct PendingWrite {
        Optional<Utf16String> key;
        Optional<Utf16String> new_value;
    };

    // An entry's quota size is kept beside its value, so overwriting or removing it never measures the old value again.
    struct Entry {
        Utf16String value;
        u64 quota_size { 0 };
    };

    // False when the owner couldn't resolve the requester's area; that requester is then not asked about again, but
    // another requester of the same mirror still can be (the first may have been a page on its way out).
    bool ensure_primed(Requester const&);
    void apply_set(Utf16String const& key, Utf16String const& value, u64 quota_size);
    void apply_remove(Utf16String const& key);
    void apply_clear();
    void push_pending_write(Optional<Utf16String> key, Optional<Utf16String> new_value);
    PendingWrite pop_pending_write();
    void reapply_pending_writes();
    void reset_to(Vector<Utf16String> keys, Vector<Utf16String> values);
    void apply_remote_change(Optional<Utf16String> const& key, Optional<Utf16String> const& new_value);

    StorageEndpointType m_endpoint_type;
    Optional<PageId> m_session_page;
    StorageKey m_storage_key;
    Optional<u64> m_quota;
    String m_registry_key;

    OrderedHashMap<Utf16String, Entry> m_entries;
    u64 m_quota_used { 0 };
    Optional<StorageAreaId> m_id;
    bool m_primed { false };
    HashTable<String> m_failed_requesters;

    // The writes the owner hasn't answered yet are the ones from m_first_pending_write on. Answered ones stay in place
    // until the queue drains, so a script that queues thousands of writes in one task costs nothing more per answer.
    Vector<PendingWrite> m_pending_writes;
    size_t m_first_pending_write { 0 };
    HashMap<Utf16String, u32> m_pending_count_by_key;
    u32 m_pending_clear_count { 0 };
};

}
