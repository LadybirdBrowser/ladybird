/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <RequestServer/Cache/CacheIndex.h>
#include <RequestServer/Cache/Utilities.h>
#include <RequestServer/Cache/Version.h>

namespace RequestServer {

static constexpr u32 CACHE_METADATA_KEY = 12389u;

static ByteString serialize_headers(HTTP::HeaderMap const& headers)
{
    StringBuilder builder;

    for (auto const& header : headers.headers()) {
        if (is_header_exempted_from_storage(header.name))
            continue;

        builder.append(header.name);
        builder.append(':');
        builder.append(header.value);
        builder.append('\n');
    }

    return builder.to_byte_string();
}

static HTTP::HeaderMap deserialize_headers(StringView serialized_headers)
{
    HTTP::HeaderMap headers;

    serialized_headers.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView serialized_header) {
        auto index = serialized_header.find(':');
        if (!index.has_value())
            return;

        auto name = serialized_header.substring_view(0, *index).trim_whitespace();
        if (is_header_exempted_from_storage(name))
            return;

        auto value = serialized_header.substring_view(*index + 1).trim_whitespace();
        headers.set(name, value);
    });

    return headers;
}

ErrorOr<CacheIndex> CacheIndex::create(Database::Database& database)
{
    auto create_cache_metadata_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS CacheMetadata (
            metadata_key INTEGER,
            version INTEGER,
            PRIMARY KEY(metadata_key)
        );
    )#"sv));
    database.execute_statement(create_cache_metadata_table, {});

    auto read_cache_version = TRY(database.prepare_statement("SELECT version FROM CacheMetadata WHERE metadata_key = ?;"sv));
    auto cache_version = 0u;

    database.execute_statement(
        read_cache_version,
        [&](auto statement_id) { cache_version = database.result_column<u32>(statement_id, 0); },
        CACHE_METADATA_KEY);

    if (cache_version != CACHE_VERSION) {
        dbgln("\033[31;1mDisk cache version mismatch:\033[0m stored version = {}, new version = {}", cache_version, CACHE_VERSION);

        // FIXME: We should more elegantly handle minor changes, i.e. use ALTER TABLE to add fields to CacheIndex.
        auto delete_cache_index_table = TRY(database.prepare_statement("DROP TABLE IF EXISTS CacheIndex;"sv));
        database.execute_statement(delete_cache_index_table, {});

        auto set_cache_version = TRY(database.prepare_statement("INSERT OR REPLACE INTO CacheMetadata VALUES (?, ?);"sv));
        database.execute_statement(set_cache_version, {}, CACHE_METADATA_KEY, CACHE_VERSION);
    }

    auto create_cache_index_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS CacheIndex (
            cache_key INTEGER,
            url TEXT,
            response_headers TEXT,
            data_size INTEGER,
            request_time INTEGER,
            response_time INTEGER,
            last_access_time INTEGER,
            PRIMARY KEY(cache_key)
        );
    )#"sv));
    database.execute_statement(create_cache_index_table, {});

    Statements statements {};
    statements.insert_entry = TRY(database.prepare_statement("INSERT OR REPLACE INTO CacheIndex VALUES (?, ?, ?, ?, ?, ?, ?);"sv));
    statements.remove_entry = TRY(database.prepare_statement("DELETE FROM CacheIndex WHERE cache_key = ?;"sv));
    statements.remove_entries_accessed_since = TRY(database.prepare_statement("DELETE FROM CacheIndex WHERE last_access_time >= ? RETURNING cache_key;"sv));
    statements.select_entry = TRY(database.prepare_statement("SELECT * FROM CacheIndex WHERE cache_key = ?;"sv));
    statements.update_response_headers = TRY(database.prepare_statement("UPDATE CacheIndex SET response_headers = ? WHERE cache_key = ?;"sv));
    statements.update_last_access_time = TRY(database.prepare_statement("UPDATE CacheIndex SET last_access_time = ? WHERE cache_key = ?;"sv));
    statements.estimate_cache_size_accessed_since = TRY(database.prepare_statement("SELECT SUM(data_size) + SUM(OCTET_LENGTH(response_headers)) FROM CacheIndex WHERE last_access_time >= ?;"sv));

    return CacheIndex { database, statements };
}

CacheIndex::CacheIndex(Database::Database& database, Statements statements)
    : m_database(database)
    , m_statements(statements)
{
}

void CacheIndex::create_entry(u64 cache_key, String url, HTTP::HeaderMap response_headers, u64 data_size, UnixDateTime request_time, UnixDateTime response_time)
{
    auto now = UnixDateTime::now();

    Entry entry {
        .cache_key = cache_key,
        .url = move(url),
        .response_headers = move(response_headers),
        .data_size = data_size,
        .request_time = request_time,
        .response_time = response_time,
        .last_access_time = now,
    };

    m_database.execute_statement(m_statements.insert_entry, {}, entry.cache_key, entry.url, serialize_headers(entry.response_headers), entry.data_size, entry.request_time, entry.response_time, entry.last_access_time);
    m_entries.set(cache_key, move(entry));
}

void CacheIndex::remove_entry(u64 cache_key)
{
    m_database.execute_statement(m_statements.remove_entry, {}, cache_key);
    m_entries.remove(cache_key);
}

void CacheIndex::remove_entries_accessed_since(UnixDateTime since, Function<void(u64 cache_key)> on_entry_removed)
{
    m_database.execute_statement(
        m_statements.remove_entries_accessed_since,
        [&](auto statement_id) {
            auto cache_key = m_database.result_column<u64>(statement_id, 0);
            m_entries.remove(cache_key);

            on_entry_removed(cache_key);
        },
        since);
}

void CacheIndex::update_response_headers(u64 cache_key, HTTP::HeaderMap response_headers)
{
    auto entry = m_entries.get(cache_key);
    if (!entry.has_value())
        return;

    m_database.execute_statement(m_statements.update_response_headers, {}, serialize_headers(response_headers), cache_key);
    entry->response_headers = move(response_headers);
}

void CacheIndex::update_last_access_time(u64 cache_key)
{
    auto entry = m_entries.get(cache_key);
    if (!entry.has_value())
        return;

    auto now = UnixDateTime::now();

    m_database.execute_statement(m_statements.update_last_access_time, {}, now, cache_key);
    entry->last_access_time = now;
}

Optional<CacheIndex::Entry&> CacheIndex::find_entry(u64 cache_key)
{
    if (auto entry = m_entries.get(cache_key); entry.has_value())
        return entry;

    m_database.execute_statement(
        m_statements.select_entry, [&](auto statement_id) {
            int column = 0;

            auto cache_key = m_database.result_column<u64>(statement_id, column++);
            auto url = m_database.result_column<String>(statement_id, column++);
            auto response_headers = m_database.result_column<ByteString>(statement_id, column++);
            auto data_size = m_database.result_column<u64>(statement_id, column++);
            auto request_time = m_database.result_column<UnixDateTime>(statement_id, column++);
            auto response_time = m_database.result_column<UnixDateTime>(statement_id, column++);
            auto last_access_time = m_database.result_column<UnixDateTime>(statement_id, column++);

            Entry entry { cache_key, move(url), deserialize_headers(response_headers), data_size, request_time, response_time, last_access_time };
            m_entries.set(cache_key, move(entry));
        },
        cache_key);

    return m_entries.get(cache_key);
}

Requests::CacheSizes CacheIndex::estimate_cache_size_accessed_since(UnixDateTime since) const
{
    Requests::CacheSizes sizes;

    m_database.execute_statement(
        m_statements.estimate_cache_size_accessed_since,
        [&](auto statement_id) { sizes.since_requested_time = m_database.result_column<u64>(statement_id, 0); },
        since);

    m_database.execute_statement(
        m_statements.estimate_cache_size_accessed_since,
        [&](auto statement_id) { sizes.total = m_database.result_column<u64>(statement_id, 0); },
        UnixDateTime::earliest());

    return sizes;
}

}
