/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWebCommon/StorageAPI/StorageAreaId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

// A process that mirrors a storage area. Every change to the area's bottle reaches it here, except one it made itself,
// which it hears acknowledged or refused instead.
class WEBVIEW_API StorageAreaListener {
public:
    virtual ~StorageAreaListener() = default;

    // A null key means the bottle was cleared. The url is the document URL of the page that made the change, or the
    // storage key when no page did.
    virtual void storage_area_changed(Web::StorageAPI::StorageAreaId, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url) = 0;
    virtual void storage_area_write_acknowledged(Web::StorageAPI::StorageAreaId) = 0;
    // A write the jar refused, with everything the bottle holds, so the writer can fall back in line with it.
    virtual void storage_area_write_refused(Web::StorageAPI::StorageAreaId, Vector<StorageItem>) = 0;
    // The bottle changed in a way not worth describing key by key; this is everything it holds now.
    virtual void storage_area_reset(Web::StorageAPI::StorageAreaId, Vector<StorageItem>) = 0;
};

// The storage areas the browser process hands out: one per bottle of one jar, each knowing the processes that mirror
// it. Every jar reports its changes here, so a change made anywhere — by a page, by DevTools, by clearing site data —
// reaches every mirror of the bottle it happened in. Nothing else has to remember to broadcast.
class WEBVIEW_API StorageAreaRegistry {
public:
    AK_ALLOC_WITH_KMALLOC;

    static StorageAreaRegistry& the();

    struct Connection {
        Web::StorageAPI::StorageAreaId id;
        Vector<StorageItem> items;
        u64 usage { 0 };
    };

    // Registers the listener at the exact point the items were read, so no change is missed or seen twice, and marks
    // the bottle accessed, since the listener's reads won't reach the jar. Every connection gets an id of its own,
    // even to an area others mirror too: a message meant for a connection that has since gone must never reach the
    // one that took its place. Only web storage is mirrored; asking for any other endpoint gets nothing, so no
    // process can write to a bottle whose endpoint has no quota.
    Optional<Connection> connect(StorageAreaListener&, StorageJar&, StorageEndpointType, String const& storage_key);
    void disconnect(StorageAreaListener&, Web::StorageAPI::StorageAreaId);
    // The listener's connection is gone.
    void remove_listener(StorageAreaListener&);

    // An area ID is a claim about the sender. One a listener was ever given stays valid for the listener's life: a
    // message it sent while the connection existed can arrive after it is gone, and such a message is dropped, not
    // held against it.
    bool was_issued_to(StorageAreaListener const&, Web::StorageAPI::StorageAreaId) const;

    // Writes from a listener. Each is dropped when the ID names no connection the listener still has.
    void set_item(StorageAreaListener&, Web::StorageAPI::StorageAreaId, Utf16String const& key, Utf16String const& value, String const& url);
    void remove_item(StorageAreaListener&, Web::StorageAPI::StorageAreaId, Utf16String const& key, String const& url);
    void clear(StorageAreaListener&, Web::StorageAPI::StorageAreaId, String const& url);

    // Every change to every jar, with the URL of the page that made it when a page did.
    Function<void(StorageJar&, StorageChange const&, Optional<String> const& url)> on_change_for_devtools;

    void did_change(Badge<StorageJar>, StorageJar&, StorageChange const&);
    void remove_areas_of(Badge<StorageJar>, StorageJar&);

private:
    struct Area {
        StorageJar* jar { nullptr };
        StorageEndpointType storage_endpoint;
        String storage_key;
        HashMap<StorageAreaListener*, Web::StorageAPI::StorageAreaId> listener_ids;
    };

    // What a live id names.
    struct LiveConnection {
        StorageJar* jar { nullptr };
        StorageBottleLocation location;
        StorageAreaListener* listener { nullptr };
    };

    struct CurrentWrite {
        StorageAreaListener* writer { nullptr };
        String url;
    };

    Area* area_for(StorageJar&, StorageBottleLocation const&);
    Area* connected_area(StorageAreaListener&, Web::StorageAPI::StorageAreaId);

    HashMap<StorageJar*, HashMap<StorageBottleLocation, Area>> m_areas;
    HashMap<Web::StorageAPI::StorageAreaId, LiveConnection> m_live_connections;
    HashMap<StorageAreaListener const*, HashTable<Web::StorageAPI::StorageAreaId>> m_issued_ids;
    Optional<CurrentWrite> m_current_write;
    u64 m_next_id { 1 };
};

}
