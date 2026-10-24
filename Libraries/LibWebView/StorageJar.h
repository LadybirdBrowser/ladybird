/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Traits.h>
#include <AK/Utf16String.h>
#include <LibDatabase/Forward.h>
#include <LibRequests/CacheSizes.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>
#include <LibWebCommon/WebView/StorageSetResult.h>
#include <LibWebView/Forward.h>

namespace WebView {

using StorageEndpointType = Web::StorageAPI::StorageEndpointType;

struct StorageLocation {
    bool operator==(StorageLocation const&) const = default;

    StorageEndpointType storage_endpoint;
    String storage_key;
    Utf16String bottle_key;
};

struct StorageBottleLocation {
    bool operator==(StorageBottleLocation const&) const = default;

    StorageEndpointType storage_endpoint;
    String storage_key;
};

// One change the jar applied, reported to whoever mirrors the bottle it happened in. Set and Removed name the key and
// carry the value it had before (and, for Set, after). Cleared names only the bottle. Reset says the bottle's content
// changed in a way too costly to enumerate key by key, so a mirror has to reload it.
struct StorageChange {
    enum class Type : u8 {
        Set,
        Removed,
        Cleared,
        Reset,
    };

    Type type;
    StorageEndpointType storage_endpoint;
    String storage_key;
    Optional<Utf16String> key;
    Optional<Utf16String> old_value;
    Optional<Utf16String> new_value;
};

struct StorageItem {
    Utf16String key;
    Utf16String value;
};

class WEBVIEW_API StorageJar {
    AK_MAKE_NONCOPYABLE(StorageJar);
    AK_MAKE_NONMOVABLE(StorageJar);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);

    static ErrorOr<NonnullOwnPtr<StorageJar>> create(Database::Database&);
    static NonnullOwnPtr<StorageJar> create();

    ~StorageJar();

    // Called after every change this jar applies, whoever asked for it, after the StorageAreaRegistry has heard of
    // it. The registry is told directly, so nothing here can be unhooked by mistake.
    Function<void(StorageChange const&)> on_change;

    Optional<Utf16String> get_item(StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key);
    Vector<StorageItem> get_all_items(StorageEndpointType storage_endpoint, String const& storage_key);
    StorageSetResult set_item(StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key, Utf16String const& bottle_value);
    void remove_item(StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& key);
    void remove_items_accessed_since(UnixDateTime);
    void clear_storage_key(StorageEndpointType storage_endpoint, String const& storage_key);
    Vector<Utf16String> get_all_keys(StorageEndpointType storage_endpoint, String const& storage_key);
    // Reads don't reach the jar once a process mirrors a bottle, so a mirror marks the bottle accessed when it loads it.
    void mark_accessed(StorageEndpointType storage_endpoint, String const& storage_key);
    u64 usage(String const& storage_key);
    u64 bottle_usage(StorageEndpointType storage_endpoint, String const& storage_key);
    Requests::CacheSizes estimate_storage_size_accessed_since(UnixDateTime since) const;

    void clone_from(StorageJar const&);

private:
    struct Statements {
        Database::StatementID get_item { 0 };
        Database::StatementID set_item { 0 };
        Database::StatementID delete_item { 0 };
        Database::StatementID delete_items_accessed_since { 0 };
        Database::StatementID bottles_accessed_since { 0 };
        Database::StatementID mark_accessed { 0 };
        Database::StatementID clear { 0 };
        Database::StatementID get_keys { 0 };
        Database::StatementID get_items { 0 };
        Database::StatementID calculate_size_excluding_bottle_key { 0 };
        Database::StatementID calculate_size { 0 };
        Database::StatementID calculate_bottle_size { 0 };
        Database::StatementID estimate_storage_size_accessed_since { 0 };
    };

    class TransientStorage {
    public:
        Optional<Utf16String> get_item(StorageLocation const& key);
        Vector<StorageItem> get_items(StorageEndpointType storage_endpoint, String const& storage_key);
        StorageSetResult set_item(StorageLocation const& key, Utf16String const& value);
        void delete_item(StorageLocation const& key);
        // Returns the bottles that lost at least one item.
        Vector<StorageBottleLocation> delete_items_accessed_since(UnixDateTime);
        void clear(StorageEndpointType storage_endpoint, String const& storage_key);
        Vector<Utf16String> get_keys(StorageEndpointType storage_endpoint, String const& storage_key);
        void mark_accessed(StorageEndpointType storage_endpoint, String const& storage_key);
        u64 usage(String const& storage_key);
        u64 bottle_usage(StorageEndpointType storage_endpoint, String const& storage_key);
        Requests::CacheSizes estimate_storage_size_accessed_since(UnixDateTime since) const;

    private:
        struct Entry {
            Utf16String value;
            UnixDateTime last_access_time;
            // Bytes this entry contributes toward its storage key's quota: Its bottle key plus its value.
            u64 quota_size { 0 };
        };

        struct Bottle {
            HashMap<Utf16String, Entry> items;
            // The sum of the quota sizes of the items, so a set need not add them up again.
            u64 usage { 0 };
        };

        HashMap<StorageBottleLocation, Bottle> m_bottles;
    };

    struct PersistedStorage {
        Optional<Utf16String> get_item(StorageLocation const& key);
        Vector<StorageItem> get_items(StorageEndpointType storage_endpoint, String const& storage_key);
        StorageSetResult set_item(StorageLocation const& key, Utf16String const& value);
        void delete_item(StorageLocation const& key);
        // Returns the bottles that lost at least one item.
        Vector<StorageBottleLocation> delete_items_accessed_since(UnixDateTime);
        void clear(StorageEndpointType storage_endpoint, String const& storage_key);
        Vector<Utf16String> get_keys(StorageEndpointType storage_endpoint, String const& storage_key);
        void mark_accessed(StorageEndpointType storage_endpoint, String const& storage_key);
        u64 usage(String const& storage_key);
        u64 bottle_usage(StorageEndpointType storage_endpoint, String const& storage_key);
        Requests::CacheSizes estimate_storage_size_accessed_since(UnixDateTime since) const;

        NonnullRefPtr<Database::Database> database;
        Statements statements;
    };

    explicit StorageJar(Optional<PersistedStorage>);

    void notify(StorageChange const&);

    Optional<PersistedStorage> m_persisted_storage;
    TransientStorage m_transient_storage;
};

}

template<>
struct AK::Traits<WebView::StorageBottleLocation> : public AK::DefaultTraits<WebView::StorageBottleLocation> {
    static unsigned hash(WebView::StorageBottleLocation const& key)
    {
        return pair_int_hash(to_underlying(key.storage_endpoint), key.storage_key.hash());
    }
};
