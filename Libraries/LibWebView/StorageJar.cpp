/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/StdLibExtras.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

// Quota size is specified in https://storage.spec.whatwg.org/#registered-storage-endpoints
static constexpr size_t LOCAL_STORAGE_QUOTA = 5 * MiB;

ErrorOr<NonnullOwnPtr<StorageJar>> StorageJar::create(Database& database)
{
    Statements statements {};

    auto create_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS WebStorage (
            storage_endpoint INTEGER,
            storage_key TEXT,
            bottle_key TEXT,
            bottle_value TEXT,
            PRIMARY KEY(storage_endpoint, storage_key, bottle_key)
        );)#"sv));
    database.execute_statement(create_table, {});

    statements.set_item = TRY(database.prepare_statement("INSERT OR REPLACE INTO WebStorage VALUES (?, ?, ?, ?);"sv));
    statements.delete_item = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.get_item = TRY(database.prepare_statement("SELECT bottle_value FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.clear = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.get_keys = TRY(database.prepare_statement("SELECT bottle_key FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.calculate_size_excluding_bottle = TRY(database.prepare_statement("SELECT SUM(LENGTH(bottle_key) + LENGTH(bottle_value)) FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key != ?;"sv));

    return adopt_own(*new StorageJar { PersistedStorage { database, statements } });
}

NonnullOwnPtr<StorageJar> StorageJar::create()
{
    return adopt_own(*new StorageJar { OptionalNone {} });
}

StorageJar::StorageJar(Optional<PersistedStorage> persisted_storage)
    : m_persisted_storage(move(persisted_storage))
{
}

StorageJar::~StorageJar() = default;

Optional<String> StorageJar::get_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key)
{
    StorageLocation storage_location { storage_endpoint, storage_key, bottle_key };
    if (m_persisted_storage.has_value())
        return m_persisted_storage->get_item(storage_location);
    return m_transient_storage.get_item(storage_location);
}

StorageOperationError StorageJar::set_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key, String const& bottle_value)
{
    StorageLocation storage_location { storage_endpoint, storage_key, bottle_key };
    if (m_persisted_storage.has_value())
        return m_persisted_storage->set_item(storage_location, bottle_value);
    return m_transient_storage.set_item(storage_location, bottle_value);
}

void StorageJar::remove_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& key)
{
    StorageLocation storage_location { storage_endpoint, storage_key, key };
    if (m_persisted_storage.has_value()) {
        m_persisted_storage->delete_item(storage_location);
    } else {
        m_transient_storage.delete_item(storage_location);
    }
}

void StorageJar::clear_storage_key(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value()) {
        m_persisted_storage->clear(storage_endpoint, storage_key);
    } else {
        m_transient_storage.clear(storage_endpoint, storage_key);
    }
}

Vector<String> StorageJar::get_all_keys(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->get_keys(storage_endpoint, storage_key);
    return m_transient_storage.get_keys(storage_endpoint, storage_key);
}

StorageOperationError StorageJar::PersistedStorage::set_item(StorageLocation const& key, String const& value)
{
    size_t current_size = 0;
    database.execute_statement(
        statements.calculate_size_excluding_bottle,
        [&](auto statement_id) {
            current_size = database.result_column<int>(statement_id, 0);
        },
        static_cast<int>(to_underlying(key.storage_endpoint)),
        key.storage_key,
        key.bottle_key);

    auto new_size = key.bottle_key.bytes().size() + value.bytes().size();
    if (current_size + new_size > LOCAL_STORAGE_QUOTA) {
        return StorageOperationError::QuotaExceededError;
    }

    database.execute_statement(
        statements.set_item,
        {},
        static_cast<int>(to_underlying(key.storage_endpoint)),
        key.storage_key,
        key.bottle_key,
        value);

    return StorageOperationError::None;
}

void StorageJar::PersistedStorage::delete_item(StorageLocation const& key)
{
    database.execute_statement(
        statements.delete_item,
        {},
        static_cast<int>(to_underlying(key.storage_endpoint)),
        key.storage_key,
        key.bottle_key);
}

Optional<String> StorageJar::PersistedStorage::get_item(StorageLocation const& key)
{
    Optional<String> result;
    database.execute_statement(
        statements.get_item,
        [&](auto statement_id) {
            result = database.result_column<String>(statement_id, 0);
        },
        static_cast<int>(to_underlying(key.storage_endpoint)),
        key.storage_key,
        key.bottle_key);
    return result;
}

void StorageJar::PersistedStorage::clear(StorageEndpointType storage_endpoint, String const& storage_key)
{
    database.execute_statement(
        statements.clear,
        {},
        static_cast<int>(to_underlying(storage_endpoint)),
        storage_key);
}

Vector<String> StorageJar::PersistedStorage::get_keys(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<String> keys;
    database.execute_statement(
        statements.get_keys,
        [&](auto statement_id) {
            keys.append(database.result_column<String>(statement_id, 0));
        },
        static_cast<int>(to_underlying(storage_endpoint)),
        storage_key);
    return keys;
}

StorageOperationError StorageJar::TransientStorage::set_item(StorageLocation const& key, String const& value)
{
    u64 current_size = 0;
    for (auto const& [existing_key, existing_value] : m_storage_items) {
        if (existing_key.storage_endpoint == key.storage_endpoint && existing_key.storage_key == key.storage_key && existing_key.bottle_key != key.bottle_key) {
            current_size += existing_key.bottle_key.bytes().size();
            current_size += existing_value.bytes().size();
        }
    }

    auto new_size = key.bottle_key.bytes().size() + value.bytes().size();
    if (current_size + new_size > LOCAL_STORAGE_QUOTA) {
        return StorageOperationError::QuotaExceededError;
    }

    m_storage_items.set(key, value);
    return StorageOperationError::None;
}

Optional<String> StorageJar::TransientStorage::get_item(StorageLocation const& key)
{
    if (auto value = m_storage_items.get(key); value.has_value())
        return value.value();
    return OptionalNone {};
}

void StorageJar::TransientStorage::delete_item(StorageLocation const& key)
{
    m_storage_items.remove(key);
}

void StorageJar::TransientStorage::clear(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<StorageLocation> keys_to_remove;
    for (auto const& [key, value] : m_storage_items) {
        if (key.storage_endpoint == storage_endpoint && key.storage_key == storage_key)
            keys_to_remove.append(key);
    }

    for (auto const& key : keys_to_remove) {
        m_storage_items.remove(key);
    }
}

Vector<String> StorageJar::TransientStorage::get_keys(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<String> keys;
    for (auto const& [key, value] : m_storage_items) {
        if (key.storage_endpoint == storage_endpoint && key.storage_key == storage_key)
            keys.append(key.bottle_key);
    }
    return keys;
}

}
