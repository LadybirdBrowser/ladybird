/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StdLibExtras.h>
#include <LibDatabase/Database.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

// Quota size is specified in https://storage.spec.whatwg.org/#registered-storage-endpoints
static constexpr size_t LOCAL_STORAGE_QUOTA = 5 * MiB;

static constexpr u32 WEB_STORAGE_SCHEMA_BASELINE_VERSION = 1u;

ErrorOr<Database::MigrationOutcome> StorageJar::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    Array<Database::Migration, 1> migrations { {
        { .version = WEB_STORAGE_SCHEMA_BASELINE_VERSION, .sql = R"#(
            CREATE TABLE IF NOT EXISTS WebStorage (
                storage_endpoint INTEGER,
                storage_key TEXT,
                bottle_key TEXT,
                bottle_value TEXT,
                last_access_time INTEGER,
                PRIMARY KEY(storage_endpoint, storage_key, bottle_key)
            );
        )#"sv },
    } };

    return database.migrate("WebStorage"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<StorageJar>> StorageJar::create(Database::Database& database)
{
    Statements statements {};

    statements.get_item = TRY(database.prepare_statement("SELECT bottle_value FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.set_item = TRY(database.prepare_statement("INSERT OR REPLACE INTO WebStorage (storage_endpoint, storage_key, bottle_key, bottle_value, last_access_time) VALUES (?, ?, ?, ?, ?);"sv));
    statements.delete_item = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.delete_items_accessed_since = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE last_access_time >= ?;"sv));
    statements.update_last_access_time = TRY(database.prepare_statement("UPDATE WebStorage SET last_access_time = ? WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.clear = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.get_keys = TRY(database.prepare_statement("SELECT bottle_key FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.calculate_size_excluding_bottle_key = TRY(database.prepare_statement("SELECT SUM(OCTET_LENGTH(bottle_key) + OCTET_LENGTH(bottle_value)) FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key != ?;"sv));
    statements.calculate_size = TRY(database.prepare_statement("SELECT COALESCE(SUM(OCTET_LENGTH(bottle_key) + OCTET_LENGTH(bottle_value)), 0) FROM WebStorage WHERE storage_key = ?;"sv));
    statements.estimate_storage_size_accessed_since = TRY(database.prepare_statement("SELECT SUM(OCTET_LENGTH(storage_key)) + SUM(OCTET_LENGTH(bottle_key)) + SUM(OCTET_LENGTH(bottle_value)) FROM WebStorage WHERE last_access_time >= ?;"sv));

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

StorageSetResult StorageJar::set_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key, String const& bottle_value)
{
    StorageLocation storage_location { storage_endpoint, storage_key, bottle_key };

    if (m_persisted_storage.has_value())
        return m_persisted_storage->set_item(storage_location, bottle_value);
    return m_transient_storage.set_item(storage_location, bottle_value);
}

void StorageJar::remove_item(StorageEndpointType storage_endpoint, String const& storage_key, String const& key)
{
    StorageLocation storage_location { storage_endpoint, storage_key, key };

    if (m_persisted_storage.has_value())
        m_persisted_storage->delete_item(storage_location);
    else
        m_transient_storage.delete_item(storage_location);
}

void StorageJar::remove_items_accessed_since(UnixDateTime since)
{
    if (m_persisted_storage.has_value())
        m_persisted_storage->delete_items_accessed_since(since);
    else
        m_transient_storage.delete_items_accessed_since(since);
}

void StorageJar::clear_storage_key(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value())
        m_persisted_storage->clear(storage_endpoint, storage_key);
    else
        m_transient_storage.clear(storage_endpoint, storage_key);
}

Vector<String> StorageJar::get_all_keys(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->get_keys(storage_endpoint, storage_key);
    return m_transient_storage.get_keys(storage_endpoint, storage_key);
}

u64 StorageJar::usage(String const& storage_key)
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->usage(storage_key);
    return m_transient_storage.usage(storage_key);
}

Requests::CacheSizes StorageJar::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->estimate_storage_size_accessed_since(since);
    return m_transient_storage.estimate_storage_size_accessed_since(since);
}

Optional<String> StorageJar::TransientStorage::get_item(StorageLocation const& key)
{
    if (auto entry = m_storage_items.get(key); entry.has_value()) {
        entry->last_access_time = UnixDateTime::now();
        return entry->value;
    }

    return {};
}

StorageSetResult StorageJar::TransientStorage::set_item(StorageLocation const& key, String const& value)
{
    auto old_value = get_item(key);

    u64 current_size = 0;

    for (auto const& [existing_key, existing_entry] : m_storage_items) {
        if (existing_key.storage_endpoint == key.storage_endpoint && existing_key.storage_key == key.storage_key && existing_key.bottle_key != key.bottle_key) {
            current_size += existing_key.bottle_key.bytes().size();
            current_size += existing_entry.value.bytes().size();
        }
    }

    auto new_size = key.bottle_key.bytes().size() + value.bytes().size();
    if (current_size + new_size > LOCAL_STORAGE_QUOTA)
        return StorageOperationError::QuotaExceededError;

    m_storage_items.set(key, { value, UnixDateTime::now() });
    return old_value;
}

void StorageJar::TransientStorage::delete_item(StorageLocation const& key)
{
    m_storage_items.remove(key);
}

void StorageJar::TransientStorage::delete_items_accessed_since(UnixDateTime since)
{
    m_storage_items.remove_all_matching([&](auto const&, auto const& entry) {
        return entry.last_access_time >= since;
    });
}

void StorageJar::TransientStorage::clear(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<StorageLocation> keys_to_remove;
    for (auto const& [key, value] : m_storage_items) {
        if (key.storage_endpoint == storage_endpoint && key.storage_key == storage_key)
            keys_to_remove.append(key);
    }

    for (auto const& key : keys_to_remove)
        m_storage_items.remove(key);
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

Requests::CacheSizes StorageJar::TransientStorage::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    Requests::CacheSizes sizes;

    for (auto const& [key, entry] : m_storage_items) {
        auto size = key.storage_key.byte_count() + key.bottle_key.byte_count() + entry.value.byte_count();
        sizes.total += size;

        if (entry.last_access_time >= since)
            sizes.since_requested_time += size;
    }

    return sizes;
}

Optional<String> StorageJar::PersistedStorage::get_item(StorageLocation const& key)
{
    Optional<String> result;

    database.execute_statement(
        statements.get_item,
        [&](auto statement_id) {
            result = database.result_column<String>(statement_id, 0);
        },
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key);

    if (result.has_value()) {
        database.execute_statement(
            statements.update_last_access_time,
            {},
            UnixDateTime::now(),
            to_underlying(key.storage_endpoint),
            key.storage_key,
            key.bottle_key);
    }

    return result;
}

StorageSetResult StorageJar::PersistedStorage::set_item(StorageLocation const& key, String const& value)
{
    auto old_value = get_item(key);

    size_t current_size = 0;
    database.execute_statement(
        statements.calculate_size_excluding_bottle_key,
        [&](auto statement_id) {
            current_size = database.result_column<int>(statement_id, 0);
        },
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key);

    auto new_size = key.bottle_key.bytes().size() + value.bytes().size();
    if (current_size + new_size > LOCAL_STORAGE_QUOTA)
        return StorageOperationError::QuotaExceededError;

    database.execute_statement(
        statements.set_item,
        {},
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key,
        value,
        UnixDateTime::now());

    return old_value;
}

void StorageJar::PersistedStorage::delete_item(StorageLocation const& key)
{
    database.execute_statement(
        statements.delete_item,
        {},
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key);
}

void StorageJar::PersistedStorage::delete_items_accessed_since(UnixDateTime since)
{
    database.execute_statement(statements.delete_items_accessed_since, {}, since);
}

void StorageJar::PersistedStorage::clear(StorageEndpointType storage_endpoint, String const& storage_key)
{
    database.execute_statement(
        statements.clear,
        {},
        to_underlying(storage_endpoint),
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
        to_underlying(storage_endpoint),
        storage_key);

    return keys;
}

u64 StorageJar::PersistedStorage::usage(String const& storage_key)
{
    u64 current_size_in_bytes = 0;
    database.execute_statement(
        statements.calculate_size,
        [&](auto statement_id) {
            current_size_in_bytes = database.result_column<u64>(statement_id, 0);
        },
        storage_key);
    return current_size_in_bytes;
}

Requests::CacheSizes StorageJar::PersistedStorage::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    Requests::CacheSizes sizes;

    database.execute_statement(
        statements.estimate_storage_size_accessed_since,
        [&](auto statement_id) { sizes.since_requested_time = database.result_column<u64>(statement_id, 0); },
        since);

    database.execute_statement(
        statements.estimate_storage_size_accessed_since,
        [&](auto statement_id) { sizes.total = database.result_column<u64>(statement_id, 0); },
        UnixDateTime::earliest());

    return sizes;
}

u64 StorageJar::TransientStorage::usage(String const& storage_key)
{
    u64 current_size_in_bytes = 0;
    for (auto const& [key, entry] : m_storage_items) {
        if (key.storage_key == storage_key) {
            current_size_in_bytes += key.bottle_key.bytes().size();
            current_size_in_bytes += entry.value.bytes().size();
        }
    }
    return current_size_in_bytes;
}

}
