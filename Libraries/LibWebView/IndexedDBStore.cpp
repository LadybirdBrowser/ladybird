/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWebView/IndexedDBStore.h>

namespace WebView {

static constexpr u32 INDEXED_DB_SCHEMA_BASELINE_VERSION = 1u;
static constexpr u32 INDEXED_DB_SCHEMA_LAST_WRITE_VERSION = 2u;

ErrorOr<Database::MigrationOutcome> IndexedDBStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = INDEXED_DB_SCHEMA_BASELINE_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS IndexedDBDatabases (
                    storage_key TEXT NOT NULL,
                    name BLOB NOT NULL,
                    metadata BLOB NOT NULL,
                    PRIMARY KEY(storage_key, name)
                );
                CREATE TABLE IF NOT EXISTS IndexedDBRecords (
                    storage_key TEXT NOT NULL,
                    database_name BLOB NOT NULL,
                    store INTEGER NOT NULL,
                    key BLOB NOT NULL,
                    value BLOB NOT NULL,
                    index_keys BLOB NOT NULL,
                    PRIMARY KEY(storage_key, database_name, store, key)
                );
            )#"sv,
        },
        {
            // When each database last changed, in milliseconds since the epoch, so clearing the site data of a time
            // range finds the storage keys whose databases changed in it.
            .version = INDEXED_DB_SCHEMA_LAST_WRITE_VERSION,
            .sql = "ALTER TABLE IndexedDBDatabases ADD COLUMN last_write INTEGER NOT NULL DEFAULT 0;"sv,
        },
    });

    return database.migrate("IndexedDB"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<IndexedDBStore>> IndexedDBStore::create(Database::Database& database)
{
    Statements statements {};

    statements.select_databases = TRY(database.prepare_statement("SELECT name, metadata FROM IndexedDBDatabases WHERE storage_key = ?;"sv));
    statements.select_records = TRY(database.prepare_statement("SELECT database_name, store, key, value, index_keys FROM IndexedDBRecords WHERE storage_key = ?;"sv));
    statements.put_database = TRY(database.prepare_statement("INSERT OR REPLACE INTO IndexedDBDatabases (storage_key, name, metadata) VALUES (?, ?, ?);"sv));
    statements.put_record = TRY(database.prepare_statement("INSERT OR REPLACE INTO IndexedDBRecords (storage_key, database_name, store, key, value, index_keys) VALUES (?, ?, ?, ?, ?, ?);"sv));
    statements.delete_record = TRY(database.prepare_statement("DELETE FROM IndexedDBRecords WHERE storage_key = ? AND database_name = ? AND store = ? AND key = ?;"sv));
    statements.delete_store = TRY(database.prepare_statement("DELETE FROM IndexedDBRecords WHERE storage_key = ? AND database_name = ? AND store = ?;"sv));
    statements.delete_database = TRY(database.prepare_statement("DELETE FROM IndexedDBDatabases WHERE storage_key = ? AND name = ?;"sv));
    statements.delete_database_records = TRY(database.prepare_statement("DELETE FROM IndexedDBRecords WHERE storage_key = ? AND database_name = ?;"sv));
    statements.set_last_write = TRY(database.prepare_statement("UPDATE IndexedDBDatabases SET last_write = ? WHERE storage_key = ? AND name = ?;"sv));
    statements.select_storage_keys_written_since = TRY(database.prepare_statement("SELECT DISTINCT storage_key FROM IndexedDBDatabases WHERE last_write >= ?;"sv));
    statements.delete_storage_key_databases = TRY(database.prepare_statement("DELETE FROM IndexedDBDatabases WHERE storage_key = ?;"sv));
    statements.delete_storage_key_records = TRY(database.prepare_statement("DELETE FROM IndexedDBRecords WHERE storage_key = ?;"sv));
    statements.estimate_size_written_since = TRY(database.prepare_statement(R"#(
        SELECT
            (SELECT COALESCE(SUM(OCTET_LENGTH(storage_key) + OCTET_LENGTH(name) + OCTET_LENGTH(metadata)), 0) FROM IndexedDBDatabases WHERE storage_key IN (SELECT storage_key FROM IndexedDBDatabases WHERE last_write >= ?))
          + (SELECT COALESCE(SUM(OCTET_LENGTH(storage_key) + OCTET_LENGTH(database_name) + OCTET_LENGTH(key) + OCTET_LENGTH(value) + OCTET_LENGTH(index_keys)), 0) FROM IndexedDBRecords WHERE storage_key IN (SELECT storage_key FROM IndexedDBDatabases WHERE last_write >= ?));
    )#"sv));

    return adopt_own(*new IndexedDBStore { database, statements });
}

IndexedDBStore::IndexedDBStore(NonnullRefPtr<Database::Database> database, Statements statements)
    : m_database(move(database))
    , m_statements(statements)
{
}

bool IndexedDBStore::load(StringView storage_key_view, OnDatabase const& on_database, OnRecord const& on_record)
{
    auto storage_key = MUST(String::from_utf8(storage_key_view));

    // NB: Nothing is handed over unless every row could be read: A database without some of its records would have
    //     its next write replace rows that this didn't read.
    struct DatabaseRow {
        ByteBuffer name;
        ByteBuffer metadata;
    };
    struct RecordRow {
        ByteBuffer database;
        u64 store { 0 };
        ByteBuffer key;
        ByteBuffer value;
        ByteBuffer index_keys;
    };
    Vector<DatabaseRow> databases;
    Vector<RecordRow> records;

    // NB: Both reads see one committed state, so the records match the shapes even if another process on the same
    //     profile writes in between.
    auto result = m_database->read_transaction([&]() -> ErrorOr<void> {
        TRY(m_database->try_execute_statement(
            m_statements.select_databases,
            [&](auto statement_id) -> ErrorOr<void> {
                databases.append({ m_database->result_column<ByteBuffer>(statement_id, 0), m_database->result_column<ByteBuffer>(statement_id, 1) });
                return {};
            },
            storage_key));
        TRY(m_database->try_execute_statement(
            m_statements.select_records,
            [&](auto statement_id) -> ErrorOr<void> {
                auto store = m_database->result_column<i64>(statement_id, 1);
                if (store < 0)
                    return Error::from_string_literal("An IndexedDB record has a negative object store id");
                records.append({
                    m_database->result_column<ByteBuffer>(statement_id, 0),
                    static_cast<u64>(store),
                    m_database->result_column<ByteBuffer>(statement_id, 2),
                    m_database->result_column<ByteBuffer>(statement_id, 3),
                    m_database->result_column<ByteBuffer>(statement_id, 4),
                });
                return {};
            },
            storage_key));
        return {};
    });
    if (result.is_error()) {
        warnln("Unable to load IndexedDB databases: {}", result.error());
        return false;
    }

    for (auto const& database : databases)
        on_database(database.name, database.metadata);
    for (auto const& record : records)
        on_record(record.database, record.store, record.key, record.value, record.index_keys);
    return true;
}

void IndexedDBStore::begin_write(StringView storage_key, ReadonlyBytes database)
{
    m_storage_key = MUST(String::from_utf8(storage_key));
    m_database_name = MUST(ByteBuffer::copy(database));
    m_pending_writes.clear();
}

void IndexedDBStore::put_metadata(ReadonlyBytes metadata)
{
    m_pending_writes.append({ .type = PendingWrite::Type::Metadata, .value = MUST(ByteBuffer::copy(metadata)) });
}

// NB: SQLite's integers are signed; an id the backend gives an object store stays far below 2^63.
static i64 store_id(u64 store)
{
    return static_cast<i64>(store);
}

void IndexedDBStore::put_record(u64 store, ReadonlyBytes key, ReadonlyBytes value, ReadonlyBytes index_keys)
{
    m_pending_writes.append({
        .type = PendingWrite::Type::Record,
        .store = store_id(store),
        .key = MUST(ByteBuffer::copy(key)),
        .value = MUST(ByteBuffer::copy(value)),
        .index_keys = MUST(ByteBuffer::copy(index_keys)),
    });
}

void IndexedDBStore::delete_record(u64 store, ReadonlyBytes key)
{
    m_pending_writes.append({ .type = PendingWrite::Type::DeleteRecord, .store = store_id(store), .key = MUST(ByteBuffer::copy(key)) });
}

void IndexedDBStore::delete_store(u64 store)
{
    m_pending_writes.append({ .type = PendingWrite::Type::DeleteStore, .store = store_id(store) });
}

void IndexedDBStore::delete_database()
{
    m_pending_writes.append({ .type = PendingWrite::Type::DeleteDatabase });
}

bool IndexedDBStore::end_write()
{
    auto writes = move(m_pending_writes);
    auto result = m_database->transaction([&]() -> ErrorOr<void> {
        ReadonlyBytes database_name = m_database_name;
        for (auto const& write : writes) {
            switch (write.type) {
            case PendingWrite::Type::Metadata:
                TRY(m_database->try_execute_statement(m_statements.put_database, {}, m_storage_key, database_name, write.value.bytes()));
                break;
            case PendingWrite::Type::Record:
                TRY(m_database->try_execute_statement(m_statements.put_record, {}, m_storage_key, database_name, write.store, write.key.bytes(), write.value.bytes(), write.index_keys.bytes()));
                break;
            case PendingWrite::Type::DeleteRecord:
                TRY(m_database->try_execute_statement(m_statements.delete_record, {}, m_storage_key, database_name, write.store, write.key.bytes()));
                break;
            case PendingWrite::Type::DeleteStore:
                TRY(m_database->try_execute_statement(m_statements.delete_store, {}, m_storage_key, database_name, write.store));
                break;
            case PendingWrite::Type::DeleteDatabase:
                TRY(m_database->try_execute_statement(m_statements.delete_database, {}, m_storage_key, database_name));
                TRY(m_database->try_execute_statement(m_statements.delete_database_records, {}, m_storage_key, database_name));
                break;
            }
        }
        TRY(m_database->try_execute_statement(m_statements.set_last_write, {}, UnixDateTime::now().milliseconds_since_epoch(), m_storage_key, database_name));
        return {};
    });
    if (result.is_error()) {
        warnln("Unable to persist an IndexedDB change: {}", result.error());
        return false;
    }
    return true;
}

Vector<String> IndexedDBStore::storage_keys_written_since(UnixDateTime since) const
{
    Vector<String> storage_keys;
    auto result = m_database->try_execute_statement(
        m_statements.select_storage_keys_written_since,
        [&](auto statement_id) -> ErrorOr<void> {
            storage_keys.append(m_database->result_column<String>(statement_id, 0));
            return {};
        },
        since.milliseconds_since_epoch());
    if (result.is_error())
        warnln("Unable to list the storage keys of IndexedDB databases: {}", result.error());
    return storage_keys;
}

bool IndexedDBStore::delete_storage_key(StringView storage_key_view)
{
    auto storage_key = MUST(String::from_utf8(storage_key_view));
    auto result = m_database->transaction([&]() -> ErrorOr<void> {
        TRY(m_database->try_execute_statement(m_statements.delete_storage_key_databases, {}, storage_key));
        TRY(m_database->try_execute_statement(m_statements.delete_storage_key_records, {}, storage_key));
        return {};
    });
    if (result.is_error()) {
        warnln("Unable to delete the IndexedDB databases of {} from the profile: {}", storage_key, result.error());
        return false;
    }
    return true;
}

Requests::CacheSizes IndexedDBStore::estimate_size_written_since(UnixDateTime since) const
{
    auto estimate = [&](i64 since_milliseconds) -> u64 {
        u64 size = 0;
        auto result = m_database->try_execute_statement(
            m_statements.estimate_size_written_since,
            [&](auto statement_id) -> ErrorOr<void> {
                size = static_cast<u64>(max<i64>(m_database->result_column<i64>(statement_id, 0), 0));
                return {};
            },
            since_milliseconds, since_milliseconds);
        if (result.is_error())
            warnln("Unable to estimate the size of IndexedDB databases: {}", result.error());
        return size;
    };
    Requests::CacheSizes sizes;
    sizes.since_requested_time = estimate(since.milliseconds_since_epoch());
    sizes.total = estimate(0);
    return sizes;
}

}
