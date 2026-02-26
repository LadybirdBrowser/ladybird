/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheIndex.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/Cache/Version.h>

namespace HTTP {

static constexpr u32 CACHE_METADATA_KEY = 12389u;

static ByteString serialize_headers(HeaderList const& headers)
{
    StringBuilder builder;

    for (auto const& header : headers) {
        builder.append(header.name);
        builder.append(':');
        builder.append(header.value);
        builder.append('\n');
    }

    return builder.to_byte_string();
}

static NonnullRefPtr<HeaderList> deserialize_headers(StringView serialized_headers)
{
    auto headers = HeaderList::create();

    serialized_headers.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView serialized_header) {
        auto index = serialized_header.find(':');
        if (!index.has_value())
            return;

        auto name = serialized_header.substring_view(0, *index);
        if (is_header_exempted_from_storage(name))
            return;

        auto value = serialized_header.substring_view(*index + 1);
        headers->append({ name, value });
    });

    return headers;
}

ErrorOr<CacheIndex> CacheIndex::create(Database::Database& database, LexicalPath const& cache_directory)
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
        if (cache_version != 0)
            dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mDisk cache version mismatch:\033[0m stored version = {}, new version = {}", cache_version, CACHE_VERSION);

        // FIXME: We should more elegantly handle minor changes, i.e. use ALTER TABLE to add fields to CacheIndex.
        auto delete_cache_index_table = TRY(database.prepare_statement("DROP TABLE IF EXISTS CacheIndex;"sv));
        database.execute_statement(delete_cache_index_table, {});

        auto set_cache_version = TRY(database.prepare_statement("INSERT OR REPLACE INTO CacheMetadata VALUES (?, ?);"sv));
        database.execute_statement(set_cache_version, {}, CACHE_METADATA_KEY, CACHE_VERSION);
    }

    auto create_cache_index_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS CacheIndex (
            cache_key INTEGER,
            vary_key INTEGER,
            url TEXT,
            request_headers BLOB,
            response_headers BLOB,
            data_size INTEGER,
            request_time INTEGER,
            response_time INTEGER,
            last_access_time INTEGER,
            PRIMARY KEY(cache_key, vary_key)
        );
    )#"sv));
    database.execute_statement(create_cache_index_table, {});

    Statements statements {};
    statements.insert_entry = TRY(database.prepare_statement("INSERT OR REPLACE INTO CacheIndex VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"sv));
    statements.remove_entry = TRY(database.prepare_statement("DELETE FROM CacheIndex WHERE cache_key = ? AND vary_key = ?;"sv));
    statements.remove_entries_accessed_since = TRY(database.prepare_statement("DELETE FROM CacheIndex WHERE last_access_time >= ? RETURNING cache_key, vary_key;"sv));
    statements.select_entries = TRY(database.prepare_statement("SELECT * FROM CacheIndex WHERE cache_key = ?;"sv));
    statements.update_response_headers = TRY(database.prepare_statement("UPDATE CacheIndex SET response_headers = ? WHERE cache_key = ? AND vary_key = ?;"sv));
    statements.update_last_access_time = TRY(database.prepare_statement("UPDATE CacheIndex SET last_access_time = ? WHERE cache_key = ? AND vary_key = ?;"sv));

    statements.remove_entries_exceeding_cache_limit = TRY(database.prepare_statement(R"#(
        WITH RankedCacheIndex AS (
            SELECT
                cache_key,
                vary_key,
                SUM(data_size + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers))
                    OVER (ORDER BY last_access_time DESC)
                    AS cumulative_estimated_size
            FROM CacheIndex
        )
        DELETE FROM CacheIndex
        WHERE (cache_key, vary_key) IN (
            SELECT cache_key, vary_key
            FROM RankedCacheIndex
            WHERE cumulative_estimated_size > ?
        )
        RETURNING cache_key, vary_key;
    )#"sv));

    statements.estimate_cache_size_accessed_since = TRY(database.prepare_statement(R"#(
        SELECT SUM(data_size + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers))
        FROM CacheIndex
        WHERE last_access_time >= ?;
    )#"sv));

    auto disk_space = TRY(FileSystem::compute_disk_space(cache_directory));
    auto maximum_disk_cache_size = compute_maximum_disk_cache_size(disk_space.free_bytes);

    Limits limits {
        .free_disk_space = disk_space.free_bytes,
        .maximum_disk_cache_size = maximum_disk_cache_size,
        .maximum_disk_cache_entry_size = compute_maximum_disk_cache_entry_size(maximum_disk_cache_size),
    };

    return CacheIndex { database, statements, limits };
}

CacheIndex::CacheIndex(Database::Database& database, Statements statements, Limits limits)
    : m_database(database)
    , m_statements(statements)
    , m_limits(limits)
{
}

ErrorOr<void> CacheIndex::create_entry(u64 cache_key, u64 vary_key, String url, NonnullRefPtr<HeaderList> request_headers, NonnullRefPtr<HeaderList> response_headers, u64 data_size, UnixDateTime request_time, UnixDateTime response_time)
{
    auto now = UnixDateTime::now();

    auto remove_exempted_headers = [](HeaderList& headers) {
        headers.delete_all_matching([&](auto const& header) {
            return is_header_exempted_from_storage(header.name);
        });
    };

    remove_exempted_headers(request_headers);
    remove_exempted_headers(response_headers);

    auto serialized_request_headers = serialize_headers(request_headers);
    auto serialized_response_headers = serialize_headers(response_headers);

    if (data_size + serialized_request_headers.length() + serialized_response_headers.length() > m_limits.maximum_disk_cache_entry_size)
        return Error::from_string_literal("Cache entry size exceeds allowed maximum");

    Entry entry {
        .vary_key = vary_key,
        .url = move(url),
        .request_headers = move(request_headers),
        .response_headers = move(response_headers),
        .data_size = data_size,
        .request_time = request_time,
        .response_time = response_time,
        .last_access_time = now,
    };

    m_database->execute_statement(m_statements.insert_entry, {}, cache_key, vary_key, entry.url, serialized_request_headers, serialized_response_headers, entry.data_size, entry.request_time, entry.response_time, entry.last_access_time);
    m_entries.ensure(cache_key).append(move(entry));

    return {};
}

void CacheIndex::remove_entry(u64 cache_key, u64 vary_key)
{
    m_database->execute_statement(m_statements.remove_entry, {}, cache_key, vary_key);
    delete_entry(cache_key, vary_key);
}

void CacheIndex::remove_entries_exceeding_cache_limit(Function<void(u64 cache_key, u64 vary_key)> on_entry_removed)
{
    m_database->execute_statement(
        m_statements.remove_entries_exceeding_cache_limit,
        [&](auto statement_id) {
            auto cache_key = m_database->result_column<u64>(statement_id, 0);
            auto vary_key = m_database->result_column<u64>(statement_id, 1);
            delete_entry(cache_key, vary_key);

            if (on_entry_removed)
                on_entry_removed(cache_key, vary_key);
        },
        m_limits.maximum_disk_cache_size);
}

void CacheIndex::remove_entries_accessed_since(UnixDateTime since, Function<void(u64 cache_key, u64 vary_key)> on_entry_removed)
{
    m_database->execute_statement(
        m_statements.remove_entries_accessed_since,
        [&](auto statement_id) {
            auto cache_key = m_database->result_column<u64>(statement_id, 0);
            auto vary_key = m_database->result_column<u64>(statement_id, 1);
            delete_entry(cache_key, vary_key);

            if (on_entry_removed)
                on_entry_removed(cache_key, vary_key);
        },
        since);
}

void CacheIndex::update_response_headers(u64 cache_key, u64 vary_key, NonnullRefPtr<HeaderList> response_headers)
{
    auto entry = get_entry(cache_key, vary_key);
    if (!entry.has_value())
        return;

    m_database->execute_statement(m_statements.update_response_headers, {}, serialize_headers(response_headers), cache_key, vary_key);
    entry->response_headers = move(response_headers);
}

void CacheIndex::update_last_access_time(u64 cache_key, u64 vary_key)
{
    auto entry = get_entry(cache_key, vary_key);
    if (!entry.has_value())
        return;

    auto now = UnixDateTime::now();

    m_database->execute_statement(m_statements.update_last_access_time, {}, now, cache_key, vary_key);
    entry->last_access_time = now;
}

Optional<CacheIndex::Entry const&> CacheIndex::find_entry(u64 cache_key, HeaderList const& request_headers)
{
    auto& entries = m_entries.ensure(cache_key, [&]() {
        Vector<Entry> entries;

        m_database->execute_statement(
            m_statements.select_entries,
            [&](auto statement_id) {
                int column = 1; // Skip the cache_key column.

                auto vary_key = m_database->result_column<u64>(statement_id, column++);
                auto url = m_database->result_column<String>(statement_id, column++);
                auto request_headers = m_database->result_column<ByteString>(statement_id, column++);
                auto response_headers = m_database->result_column<ByteString>(statement_id, column++);
                auto data_size = m_database->result_column<u64>(statement_id, column++);
                auto request_time = m_database->result_column<UnixDateTime>(statement_id, column++);
                auto response_time = m_database->result_column<UnixDateTime>(statement_id, column++);
                auto last_access_time = m_database->result_column<UnixDateTime>(statement_id, column++);

                entries.empend(vary_key, move(url), deserialize_headers(request_headers), deserialize_headers(response_headers), data_size, request_time, response_time, last_access_time);
            },
            cache_key);

        return entries;
    });

    return find_value(entries, [&](auto const& entry) {
        return create_vary_key(request_headers, entry.response_headers) == entry.vary_key;
    });
}

Optional<CacheIndex::Entry&> CacheIndex::get_entry(u64 cache_key, u64 vary_key)
{
    auto entries = m_entries.get(cache_key);
    if (!entries.has_value())
        return {};

    return find_value(*entries, [&](auto const& entry) { return entry.vary_key == vary_key; });
}

void CacheIndex::delete_entry(u64 cache_key, u64 vary_key)
{
    auto entries = m_entries.get(cache_key);
    if (!entries.has_value())
        return;

    entries->remove_first_matching([&](auto const& entry) { return entry.vary_key == vary_key; });

    if (entries->is_empty())
        m_entries.remove(cache_key);
}

Requests::CacheSizes CacheIndex::estimate_cache_size_accessed_since(UnixDateTime since)
{
    Requests::CacheSizes sizes;

    m_database->execute_statement(
        m_statements.estimate_cache_size_accessed_since,
        [&](auto statement_id) { sizes.since_requested_time = m_database->result_column<u64>(statement_id, 0); },
        since);

    m_database->execute_statement(
        m_statements.estimate_cache_size_accessed_since,
        [&](auto statement_id) { sizes.total = m_database->result_column<u64>(statement_id, 0); },
        UnixDateTime::earliest());

    return sizes;
}

void CacheIndex::set_maximum_disk_cache_size(u64 maximum_disk_cache_size)
{
    if (maximum_disk_cache_size == m_limits.maximum_disk_cache_size)
        return;

    m_limits.maximum_disk_cache_size = compute_maximum_disk_cache_size(m_limits.free_disk_space, maximum_disk_cache_size);
    m_limits.maximum_disk_cache_entry_size = compute_maximum_disk_cache_entry_size(m_limits.maximum_disk_cache_size);
}

}
