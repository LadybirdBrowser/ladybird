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

using Web::StorageAPI::quota_for;
using Web::StorageAPI::storage_quota_size;

namespace WebView {

static constexpr u32 WEB_STORAGE_SCHEMA_BASELINE_VERSION = 2u;

ErrorOr<Database::MigrationOutcome> StorageJar::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = WEB_STORAGE_SCHEMA_BASELINE_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS WebStorage (
                    storage_endpoint INTEGER,
                    storage_key TEXT,
                    bottle_key TEXT,
                    bottle_value TEXT,
                    last_access_time INTEGER,
                    PRIMARY KEY(storage_endpoint, storage_key, bottle_key)
                );
            )#"sv,
        },
    });

    return database.migrate("WebStorage"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<StorageJar>> StorageJar::create(Database::Database& database)
{
    Statements statements {};

    statements.get_item = TRY(database.prepare_statement("SELECT bottle_value FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.set_item = TRY(database.prepare_statement("INSERT OR REPLACE INTO WebStorage (storage_endpoint, storage_key, bottle_key, bottle_value, last_access_time) VALUES (?, ?, ?, ?, ?);"sv));
    statements.delete_item = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key = ?;"sv));
    statements.delete_items_accessed_since = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE last_access_time >= ?;"sv));
    statements.bottles_accessed_since = TRY(database.prepare_statement("SELECT DISTINCT storage_endpoint, storage_key FROM WebStorage WHERE last_access_time >= ?;"sv));
    statements.mark_accessed = TRY(database.prepare_statement("UPDATE WebStorage SET last_access_time = ? WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.clear = TRY(database.prepare_statement("DELETE FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.get_keys = TRY(database.prepare_statement("SELECT bottle_key FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.get_items = TRY(database.prepare_statement("SELECT bottle_key, bottle_value FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.calculate_size_excluding_bottle_key = TRY(database.prepare_statement("SELECT COALESCE(SUM(OCTET_LENGTH(bottle_key) + OCTET_LENGTH(bottle_value)), 0) FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ? AND bottle_key != ?;"sv));
    statements.calculate_size = TRY(database.prepare_statement("SELECT COALESCE(SUM(COALESCE(OCTET_LENGTH(bottle_key), 0) + COALESCE(OCTET_LENGTH(bottle_value), 0)), 0) FROM WebStorage WHERE storage_key = ?;"sv));
    statements.calculate_bottle_size = TRY(database.prepare_statement("SELECT COALESCE(SUM(COALESCE(OCTET_LENGTH(bottle_key), 0) + COALESCE(OCTET_LENGTH(bottle_value), 0)), 0) FROM WebStorage WHERE storage_endpoint = ? AND storage_key = ?;"sv));
    statements.estimate_storage_size_accessed_since = TRY(database.prepare_statement("SELECT COALESCE(SUM(COALESCE(OCTET_LENGTH(storage_key), 0) + COALESCE(OCTET_LENGTH(bottle_key), 0) + COALESCE(OCTET_LENGTH(bottle_value), 0)), 0) FROM WebStorage WHERE last_access_time >= ?;"sv));

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

void StorageJar::notify(StorageChange const& change)
{
    if (on_change)
        on_change(change);
}

Optional<Utf16String> StorageJar::get_item(StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key)
{
    StorageLocation storage_location { storage_endpoint, storage_key, bottle_key };

    if (m_persisted_storage.has_value())
        return m_persisted_storage->get_item(storage_location);
    return m_transient_storage.get_item(storage_location);
}

Vector<StorageItem> StorageJar::get_all_items(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->get_items(storage_endpoint, storage_key);
    return m_transient_storage.get_items(storage_endpoint, storage_key);
}

StorageSetResult StorageJar::set_item(StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key, Utf16String const& bottle_value)
{
    StorageLocation storage_location { storage_endpoint, storage_key, bottle_key };

    auto result = m_persisted_storage.has_value()
        ? m_persisted_storage->set_item(storage_location, bottle_value)
        : m_transient_storage.set_item(storage_location, bottle_value);

    // A refused write and a write of the value already there change nothing, so neither is reported.
    if (auto const* old_value = result.get_pointer<Optional<Utf16String>>(); old_value && *old_value != bottle_value)
        notify({ StorageChange::Type::Set, storage_endpoint, storage_key, bottle_key, *old_value, bottle_value });
    return result;
}

void StorageJar::remove_item(StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& key)
{
    auto old_value = get_item(storage_endpoint, storage_key, key);
    if (!old_value.has_value())
        return;

    StorageLocation storage_location { storage_endpoint, storage_key, key };
    if (m_persisted_storage.has_value())
        m_persisted_storage->delete_item(storage_location);
    else
        m_transient_storage.delete_item(storage_location);
    notify({ StorageChange::Type::Removed, storage_endpoint, storage_key, key, move(old_value), {} });
}

void StorageJar::remove_items_accessed_since(UnixDateTime since)
{
    auto affected_bottles = m_persisted_storage.has_value()
        ? m_persisted_storage->delete_items_accessed_since(since)
        : m_transient_storage.delete_items_accessed_since(since);

    for (auto const& bottle : affected_bottles)
        notify({ StorageChange::Type::Reset, bottle.storage_endpoint, bottle.storage_key, {}, {}, {} });
}

void StorageJar::clear_storage_key(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (get_all_keys(storage_endpoint, storage_key).is_empty())
        return;

    if (m_persisted_storage.has_value())
        m_persisted_storage->clear(storage_endpoint, storage_key);
    else
        m_transient_storage.clear(storage_endpoint, storage_key);
    notify({ StorageChange::Type::Cleared, storage_endpoint, storage_key, {}, {}, {} });
}

void StorageJar::mark_accessed(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value())
        m_persisted_storage->mark_accessed(storage_endpoint, storage_key);
    else
        m_transient_storage.mark_accessed(storage_endpoint, storage_key);
}

Vector<Utf16String> StorageJar::get_all_keys(StorageEndpointType storage_endpoint, String const& storage_key)
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

u64 StorageJar::bottle_usage(StorageEndpointType storage_endpoint, String const& storage_key)
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->bottle_usage(storage_endpoint, storage_key);
    return m_transient_storage.bottle_usage(storage_endpoint, storage_key);
}

Requests::CacheSizes StorageJar::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    if (m_persisted_storage.has_value())
        return m_persisted_storage->estimate_storage_size_accessed_since(since);
    return m_transient_storage.estimate_storage_size_accessed_since(since);
}

void StorageJar::clone_from(StorageJar const& other)
{
    VERIFY(!m_persisted_storage.has_value());
    VERIFY(!other.m_persisted_storage.has_value());
    m_transient_storage = other.m_transient_storage;
}

Optional<Utf16String> StorageJar::TransientStorage::get_item(StorageLocation const& key)
{
    auto bottle = m_bottles.get({ key.storage_endpoint, key.storage_key });
    if (!bottle.has_value())
        return {};

    if (auto entry = bottle->items.get(key.bottle_key); entry.has_value())
        return entry->value;

    return {};
}

Vector<StorageItem> StorageJar::TransientStorage::get_items(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<StorageItem> items;
    auto bottle = m_bottles.get({ storage_endpoint, storage_key });
    if (!bottle.has_value())
        return items;

    items.ensure_capacity(bottle->items.size());
    for (auto const& [bottle_key, entry] : bottle->items)
        items.unchecked_append({ bottle_key, entry.value });
    return items;
}

StorageSetResult StorageJar::TransientStorage::set_item(StorageLocation const& key, Utf16String const& value)
{
    auto& bottle = m_bottles.ensure({ key.storage_endpoint, key.storage_key });

    Optional<Utf16String> old_value;
    auto current_size = bottle.usage;

    if (auto entry = bottle.items.get(key.bottle_key); entry.has_value()) {
        old_value = entry->value;
        current_size -= entry->quota_size;
    }

    auto new_size = storage_quota_size(key.bottle_key) + storage_quota_size(value);
    if (auto quota = quota_for(key.storage_endpoint); quota.has_value() && current_size + new_size > *quota)
        return StorageOperationError::QuotaExceededError;

    bottle.items.set(key.bottle_key, { value, UnixDateTime::now(), new_size });
    bottle.usage = current_size + new_size;
    return old_value;
}

void StorageJar::TransientStorage::delete_item(StorageLocation const& key)
{
    auto bottle = m_bottles.get({ key.storage_endpoint, key.storage_key });
    if (!bottle.has_value())
        return;

    if (auto entry = bottle->items.take(key.bottle_key); entry.has_value())
        bottle->usage -= entry->quota_size;
}

Vector<StorageBottleLocation> StorageJar::TransientStorage::delete_items_accessed_since(UnixDateTime since)
{
    Vector<StorageBottleLocation> affected_bottles;
    for (auto& [location, bottle] : m_bottles) {
        auto removed = bottle.items.remove_all_matching([&](auto const&, auto const& entry) {
            if (entry.last_access_time < since)
                return false;
            bottle.usage -= entry.quota_size;
            return true;
        });
        if (removed)
            affected_bottles.append(location);
    }
    return affected_bottles;
}

void StorageJar::TransientStorage::mark_accessed(StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto bottle = m_bottles.get({ storage_endpoint, storage_key });
    if (!bottle.has_value())
        return;

    auto now = UnixDateTime::now();
    for (auto& entry : bottle->items)
        entry.value.last_access_time = now;
}

u64 StorageJar::TransientStorage::bottle_usage(StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto bottle = m_bottles.get({ storage_endpoint, storage_key });
    return bottle.has_value() ? bottle->usage : 0;
}

void StorageJar::TransientStorage::clear(StorageEndpointType storage_endpoint, String const& storage_key)
{
    m_bottles.remove({ storage_endpoint, storage_key });
}

Vector<Utf16String> StorageJar::TransientStorage::get_keys(StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto bottle = m_bottles.get({ storage_endpoint, storage_key });
    if (!bottle.has_value())
        return {};
    return bottle->items.keys();
}

Requests::CacheSizes StorageJar::TransientStorage::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    Requests::CacheSizes sizes;

    for (auto const& [location, bottle] : m_bottles) {
        for (auto const& [bottle_key, entry] : bottle.items) {
            auto size = location.storage_key.byte_count() + entry.quota_size;
            sizes.total += size;

            if (entry.last_access_time >= since)
                sizes.since_requested_time += size;
        }
    }

    return sizes;
}

Optional<Utf16String> StorageJar::PersistedStorage::get_item(StorageLocation const& key)
{
    Optional<Utf16String> result;

    database->execute_statement(
        statements.get_item,
        [&](auto statement_id) -> ErrorOr<void> {
            result = database->result_column<Utf16String>(statement_id, 0);
            return {};
        },
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key);

    return result;
}

Vector<StorageItem> StorageJar::PersistedStorage::get_items(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<StorageItem> items;

    database->execute_statement(
        statements.get_items,
        [&](auto statement_id) -> ErrorOr<void> {
            items.append({ database->result_column<Utf16String>(statement_id, 0), database->result_column<Utf16String>(statement_id, 1) });
            return {};
        },
        to_underlying(storage_endpoint),
        storage_key);

    return items;
}

StorageSetResult StorageJar::PersistedStorage::set_item(StorageLocation const& key, Utf16String const& value)
{
    auto old_value = get_item(key);

    size_t current_size = 0;
    database->execute_statement(
        statements.calculate_size_excluding_bottle_key,
        [&](auto statement_id) -> ErrorOr<void> {
            current_size = database->result_column<i64>(statement_id, 0);
            return {};
        },
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key);

    auto new_size = storage_quota_size(key.bottle_key) + storage_quota_size(value);
    if (auto quota = quota_for(key.storage_endpoint); quota.has_value() && current_size + new_size > *quota)
        return StorageOperationError::QuotaExceededError;

    database->execute_statement(
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
    database->execute_statement(
        statements.delete_item,
        {},
        to_underlying(key.storage_endpoint),
        key.storage_key,
        key.bottle_key);
}

Vector<StorageBottleLocation> StorageJar::PersistedStorage::delete_items_accessed_since(UnixDateTime since)
{
    Vector<StorageBottleLocation> affected_bottles;

    database->execute_statement(
        statements.bottles_accessed_since,
        [&](auto statement_id) -> ErrorOr<void> {
            affected_bottles.append({ static_cast<StorageEndpointType>(database->result_column<i64>(statement_id, 0)), database->result_column<String>(statement_id, 1) });
            return {};
        },
        since);

    database->execute_statement(statements.delete_items_accessed_since, {}, since);
    return affected_bottles;
}

void StorageJar::PersistedStorage::mark_accessed(StorageEndpointType storage_endpoint, String const& storage_key)
{
    database->execute_statement(
        statements.mark_accessed,
        {},
        UnixDateTime::now(),
        to_underlying(storage_endpoint),
        storage_key);
}

u64 StorageJar::PersistedStorage::bottle_usage(StorageEndpointType storage_endpoint, String const& storage_key)
{
    u64 current_size_in_bytes = 0;
    database->execute_statement(
        statements.calculate_bottle_size,
        [&](auto statement_id) -> ErrorOr<void> {
            current_size_in_bytes = static_cast<u64>(database->result_column<i64>(statement_id, 0));
            return {};
        },
        to_underlying(storage_endpoint),
        storage_key);
    return current_size_in_bytes;
}

void StorageJar::PersistedStorage::clear(StorageEndpointType storage_endpoint, String const& storage_key)
{
    database->execute_statement(
        statements.clear,
        {},
        to_underlying(storage_endpoint),
        storage_key);
}

Vector<Utf16String> StorageJar::PersistedStorage::get_keys(StorageEndpointType storage_endpoint, String const& storage_key)
{
    Vector<Utf16String> keys;

    database->execute_statement(
        statements.get_keys,
        [&](auto statement_id) -> ErrorOr<void> {
            keys.append(database->result_column<Utf16String>(statement_id, 0));
            return {};
        },
        to_underlying(storage_endpoint),
        storage_key);

    return keys;
}

u64 StorageJar::PersistedStorage::usage(String const& storage_key)
{
    u64 current_size_in_bytes = 0;
    database->execute_statement(
        statements.calculate_size,
        [&](auto statement_id) -> ErrorOr<void> {
            current_size_in_bytes = static_cast<u64>(database->result_column<i64>(statement_id, 0));
            return {};
        },
        storage_key);
    return current_size_in_bytes;
}

Requests::CacheSizes StorageJar::PersistedStorage::estimate_storage_size_accessed_since(UnixDateTime since) const
{
    Requests::CacheSizes sizes;

    database->execute_statement(
        statements.estimate_storage_size_accessed_since,
        [&](auto statement_id) -> ErrorOr<void> { sizes.since_requested_time = static_cast<u64>(database->result_column<i64>(statement_id, 0)); return {}; },
        since);

    database->execute_statement(
        statements.estimate_storage_size_accessed_since,
        [&](auto statement_id) -> ErrorOr<void> { sizes.total = static_cast<u64>(database->result_column<i64>(statement_id, 0)); return {}; },
        UnixDateTime::earliest());

    return sizes;
}

u64 StorageJar::TransientStorage::usage(String const& storage_key)
{
    u64 current_size_in_bytes = 0;
    for (auto const& [location, bottle] : m_bottles) {
        if (location.storage_key == storage_key)
            current_size_in_bytes += bottle.usage;
    }
    return current_size_in_bytes;
}

}
