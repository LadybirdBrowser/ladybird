/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Traits.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWebView/Database.h>
#include <LibWebView/Forward.h>
#include <LibWebView/StorageOperationError.h>

namespace WebView {

using StorageEndpointType = Web::StorageAPI::StorageEndpointType;

struct StorageLocation {
    bool operator==(StorageLocation const&) const = default;

    StorageEndpointType storage_endpoint;
    String storage_key;
    String bottle_key;
};

class WEBVIEW_API StorageJar {
    AK_MAKE_NONCOPYABLE(StorageJar);
    AK_MAKE_NONMOVABLE(StorageJar);

public:
    static ErrorOr<NonnullOwnPtr<StorageJar>> create(Database&);
    static NonnullOwnPtr<StorageJar> create();

    ~StorageJar();

    Optional<String> get_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key);
    StorageOperationError set_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key, String const& bottle_value);
    void remove_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& key);
    void clear_storage_key(StorageEndpointType storage_endpoint, String const& storage_key);
    Vector<String> get_all_keys(StorageEndpointType storage_endpoint, String const& storage_key);

private:
    struct Statements {
        Database::StatementID set_item { 0 };
        Database::StatementID delete_item { 0 };
        Database::StatementID get_item { 0 };
        Database::StatementID clear { 0 };
        Database::StatementID get_keys { 0 };
        Database::StatementID calculate_size_excluding_key { 0 };
    };

    class TransientStorage {
    public:
        StorageOperationError set_item(StorageLocation const& key, String const& value);
        Optional<String> get_item(StorageLocation const& key);
        void delete_item(StorageLocation const& key);
        void clear(StorageEndpointType storage_endpoint, String const& storage_key);
        Vector<String> get_keys(StorageEndpointType storage_endpoint, String const& storage_key);

    private:
        HashMap<StorageLocation, String> m_storage_items;
    };

    struct PersistedStorage {
        StorageOperationError set_item(StorageLocation const& key, String const& value);
        Optional<String> get_item(StorageLocation const& key);
        void delete_item(StorageLocation const& key);
        void clear(StorageEndpointType storage_endpoint, String const& storage_key);
        Vector<String> get_keys(StorageEndpointType storage_endpoint, String const& storage_key);

        Database& database;
        Statements statements;
    };

    explicit StorageJar(Optional<PersistedStorage>);

    Optional<PersistedStorage> m_persisted_storage;
    TransientStorage m_transient_storage;
};

}

template<>
struct AK::Traits<WebView::StorageLocation> : public AK::DefaultTraits<WebView::StorageLocation> {
    static unsigned hash(WebView::StorageLocation const& key)
    {
        unsigned hash = 0;
        hash = pair_int_hash(hash, to_underlying(key.storage_endpoint));
        hash = pair_int_hash(hash, key.storage_key.hash());
        hash = pair_int_hash(hash, key.bottle_key.hash());
        return hash;
    }
};
