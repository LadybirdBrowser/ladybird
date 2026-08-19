/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitCast.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheIndex.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/Cache/Version.h>

namespace HTTP {

static constexpr u32 INDEX_SCHEMA_BASELINE_VERSION = 1u;

// Persist full-range hashes as signed bit patterns; keys are only compared for equality.
static i64 encode_cache_key_for_database(u64 key)
{
    return bit_cast<i64>(key);
}

static u64 decode_cache_key_from_database(i64 stored_key)
{
    return bit_cast<u64>(stored_key);
}

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

template<typename Callback>
static void for_each_cache_entry_file(Database::Database& database, Callback&& callback)
{
    auto const& index_path = database.database_path();
    if (!index_path.has_value())
        return;

    (void)Core::Directory::for_each_entry(
        index_path->parent().string(),
        static_cast<Core::DirIterator::Flags>(Core::DirIterator::SkipDots | Core::DirIterator::NoStat),
        [&](Core::DirectoryEntry const& entry, Core::Directory const& parent) -> ErrorOr<IterationDecision> {
            if (entry.type != Core::DirectoryEntry::Type::File)
                return IterationDecision::Continue;

            // Ignore INDEX.db and related files (e.g. the WAL file, INDEX.db-wal).
            if (entry.name.starts_with(index_path->basename()))
                return IterationDecision::Continue;

            callback(parent.path().append(entry.name));
            return IterationDecision::Continue;
        });
}

#if HTTP_DISK_CACHE_DEBUG

static void log_orphaned_disk_cache_entries(Database::Database& database)
{
    auto check_entry = MUST(database.prepare_statement(R"#(
        SELECT 1 FROM CacheIndex WHERE cache_key = ? AND vary_key = ? LIMIT 1;
    )#"sv));

    for_each_cache_entry_file(database, [&](LexicalPath const& cache_entry) {
        auto cache_entry_data = cache_entry_data_for_file(cache_entry);
        if (!cache_entry_data.has_value()) {
            dbgln("Unrecognized cache file: {}", cache_entry);
            return;
        }

        bool has_entry = false;
        database.execute_statement(check_entry, [&](auto) -> ErrorOr<void> { has_entry = true; return {}; }, encode_cache_key_for_database(cache_entry_data->cache_key), encode_cache_key_for_database(cache_entry_data->vary_key));

        if (!has_entry)
            dbgln("Cache file missing from cache index: {}", cache_entry);
    });
}

#endif

// The index schema version is independent of CACHE_VERSION: index-only schema changes append
// a migration here without invalidating the cache entries on disk. Entry files embed
// CACHE_VERSION in their headers and are validated when read, so an entry format break must
// append a migration that deletes the index rows referencing the now-unreadable entries.
static_assert(CACHE_VERSION == 7, "Bumping CACHE_VERSION requires appending a CacheIndex migration that deletes the index rows referencing the old entry format");

ErrorOr<Database::MigrationOutcome> CacheIndex::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = INDEX_SCHEMA_BASELINE_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS CacheIndex (
                    cache_key INTEGER,
                    vary_key INTEGER,
                    url TEXT,
                    request_headers BLOB,
                    response_headers BLOB,
                    data_size INTEGER,
                    associated_data_size INTEGER,
                    request_time INTEGER,
                    response_time INTEGER,
                    last_access_time INTEGER,
                    PRIMARY KEY(cache_key, vary_key)
                );
            )#"sv,
        },
    });

    return database.migrate("CacheIndex"sv, migrations, mode);
}

ErrorOr<CacheIndex> CacheIndex::create(Database::Database& database, LexicalPath const& cache_directory)
{
#if HTTP_DISK_CACHE_DEBUG
    log_orphaned_disk_cache_entries(database);
#endif

    Statements statements {};
    statements.insert_entry = TRY(database.prepare_statement("INSERT OR REPLACE INTO CacheIndex (cache_key, vary_key, url, request_headers, response_headers, data_size, associated_data_size, request_time, response_time, last_access_time) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"sv));
    statements.remove_entry = TRY(database.prepare_statement(R"#(
        DELETE FROM CacheIndex
        WHERE cache_key = ? AND vary_key = ?
        RETURNING MAX(data_size, 0) + MAX(associated_data_size, 0) + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers);
    )#"sv));
    statements.remove_entries_accessed_since = TRY(database.prepare_statement(R"#(
        DELETE FROM CacheIndex
        WHERE last_access_time >= ?
        RETURNING cache_key, vary_key, MAX(data_size, 0) + MAX(associated_data_size, 0) + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers);
    )#"sv));
    statements.select_entries = TRY(database.prepare_statement("SELECT vary_key, url, request_headers, response_headers, data_size, associated_data_size, request_time, response_time, last_access_time FROM CacheIndex WHERE cache_key = ?;"sv));
    statements.update_response_headers = TRY(database.prepare_statement("UPDATE CacheIndex SET response_headers = ? WHERE cache_key = ? AND vary_key = ?;"sv));
    statements.update_associated_data_size = TRY(database.prepare_statement("UPDATE CacheIndex SET associated_data_size = ? WHERE cache_key = ? AND vary_key = ?;"sv));
    statements.update_last_access_time = TRY(database.prepare_statement("UPDATE CacheIndex SET last_access_time = ? WHERE cache_key = ? AND vary_key = ?;"sv));

    statements.remove_entries_exceeding_cache_limit = TRY(database.prepare_statement(R"#(
        WITH RankedCacheIndex AS (
            SELECT
                cache_key,
                vary_key,
                SUM(MAX(data_size, 0) + MAX(associated_data_size, 0) + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers))
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
        RETURNING cache_key, vary_key, MAX(data_size, 0) + MAX(associated_data_size, 0) + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers);
    )#"sv));

    statements.estimate_cache_size_accessed_since = TRY(database.prepare_statement(R"#(
        SELECT COALESCE(SUM(MAX(data_size, 0) + MAX(associated_data_size, 0) + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers)), 0)
        FROM CacheIndex
        WHERE last_access_time >= ?;
    )#"sv));

    statements.select_total_estimated_size = TRY(database.prepare_statement(R"#(
        SELECT COALESCE(SUM(MAX(data_size, 0) + MAX(associated_data_size, 0) + OCTET_LENGTH(request_headers) + OCTET_LENGTH(response_headers)), 0)
        FROM CacheIndex;
    )#"sv));

    auto disk_space = TRY(FileSystem::compute_disk_space(cache_directory));
    auto maximum_disk_cache_size = compute_maximum_disk_cache_size(disk_space.free_bytes);

    Limits limits {
        .free_disk_space = disk_space.free_bytes,
        .maximum_disk_cache_size = maximum_disk_cache_size,
        .maximum_disk_cache_entry_size = compute_maximum_disk_cache_entry_size(maximum_disk_cache_size),
    };

    i64 total_estimated_size { 0 };
    database.execute_statement(
        statements.select_total_estimated_size,
        [&](auto statement_id) -> ErrorOr<void> { total_estimated_size = database.result_column<i64>(statement_id, 0); return {}; });

    return CacheIndex { database, statements, limits, total_estimated_size };
}

CacheIndex::CacheIndex(Database::Database& database, Statements statements, Limits limits, i64 total_estimated_size)
    : m_database(database)
    , m_statements(statements)
    , m_limits(limits)
    , m_total_estimated_size(total_estimated_size)
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

    Entry entry {
        .vary_key = vary_key,
        .url = move(url),
        .request_headers = move(request_headers),
        .response_headers = move(response_headers),
        .data_size = data_size,
        .associated_data_size = 0,
        .serialized_request_headers_size = static_cast<u64>(serialized_request_headers.length()),
        .serialized_response_headers_size = static_cast<u64>(serialized_response_headers.length()),
        .request_time = request_time,
        .response_time = response_time,
        .last_access_time = now,
    };
    Checked<u64> checked_entry_size = entry.data_size;
    checked_entry_size += entry.associated_data_size;
    checked_entry_size += entry.serialized_request_headers_size;
    checked_entry_size += entry.serialized_response_headers_size;

    if (checked_entry_size.has_overflow() || checked_entry_size.value() > static_cast<u64>(m_limits.maximum_disk_cache_entry_size))
        return Error::from_string_literal("Cache entry size exceeds allowed maximum");
    auto entry_size = checked_entry_size.value();

    m_database->execute_statement(
        m_statements.remove_entry,
        [&](auto statement_id) -> ErrorOr<void> {
            auto removed_size = m_database->result_column<i64>(statement_id, 0);
            adjust_total_estimated_size(-removed_size);
            return {};
        },
        encode_cache_key_for_database(cache_key),
        encode_cache_key_for_database(vary_key));

    auto& entries = m_entries.ensure(cache_key);
    auto existing_entry_index = entries.find_first_index_if([&](auto const& existing_entry) {
        return existing_entry.vary_key == vary_key;
    });

    m_database->execute_statement(m_statements.insert_entry, {}, encode_cache_key_for_database(cache_key), encode_cache_key_for_database(vary_key), entry.url, serialized_request_headers, serialized_response_headers, static_cast<i64>(entry.data_size), static_cast<i64>(entry.associated_data_size), entry.request_time, entry.response_time, entry.last_access_time);

    if (existing_entry_index.has_value())
        entries[*existing_entry_index] = move(entry);
    else
        entries.append(move(entry));

    adjust_total_estimated_size(static_cast<i64>(entry_size));

    return {};
}

void CacheIndex::remove_entry(u64 cache_key, u64 vary_key)
{
    m_database->execute_statement(
        m_statements.remove_entry,
        [&](auto statement_id) -> ErrorOr<void> {
            auto removed_size = m_database->result_column<i64>(statement_id, 0);
            adjust_total_estimated_size(-removed_size);
            return {};
        },
        encode_cache_key_for_database(cache_key),
        encode_cache_key_for_database(vary_key));

    delete_entry(cache_key, vary_key);
}

void CacheIndex::remove_entries_exceeding_cache_limit(Function<void(u64 cache_key, u64 vary_key)> on_entry_removed)
{
    if (m_total_estimated_size <= m_limits.maximum_disk_cache_size)
        return;

    m_database->execute_statement(
        m_statements.remove_entries_exceeding_cache_limit,
        [&](auto statement_id) -> ErrorOr<void> {
            auto cache_key = decode_cache_key_from_database(m_database->result_column<i64>(statement_id, 0));
            auto vary_key = decode_cache_key_from_database(m_database->result_column<i64>(statement_id, 1));
            auto removed_size = m_database->result_column<i64>(statement_id, 2);
            adjust_total_estimated_size(-removed_size);
            delete_entry(cache_key, vary_key);

            if (on_entry_removed)
                on_entry_removed(cache_key, vary_key);
            return {};
        },
        m_limits.maximum_disk_cache_size);
}

void CacheIndex::remove_entries_accessed_since(UnixDateTime since, Function<void(u64 cache_key, u64 vary_key)> on_entry_removed)
{
    m_database->execute_statement(
        m_statements.remove_entries_accessed_since,
        [&](auto statement_id) -> ErrorOr<void> {
            auto cache_key = decode_cache_key_from_database(m_database->result_column<i64>(statement_id, 0));
            auto vary_key = decode_cache_key_from_database(m_database->result_column<i64>(statement_id, 1));
            auto removed_size = m_database->result_column<i64>(statement_id, 2);
            adjust_total_estimated_size(-removed_size);
            delete_entry(cache_key, vary_key);

            if (on_entry_removed)
                on_entry_removed(cache_key, vary_key);
            return {};
        },
        since);
}

void CacheIndex::update_response_headers(u64 cache_key, u64 vary_key, NonnullRefPtr<HeaderList> response_headers)
{
    auto entry = get_entry(cache_key, vary_key);
    if (!entry.has_value())
        return;

    auto serialized_response_headers = serialize_headers(response_headers);
    auto serialized_response_headers_size = static_cast<u64>(serialized_response_headers.length());

    m_database->execute_statement(m_statements.update_response_headers, {}, serialized_response_headers, encode_cache_key_for_database(cache_key), encode_cache_key_for_database(vary_key));

    adjust_total_estimated_size(-static_cast<i64>(entry->serialized_response_headers_size));
    adjust_total_estimated_size(static_cast<i64>(serialized_response_headers_size));
    entry->response_headers = move(response_headers);
    entry->serialized_response_headers_size = serialized_response_headers_size;
}

ErrorOr<void> CacheIndex::update_associated_data_size(u64 cache_key, u64 vary_key, u64 associated_data_size)
{
    auto entry = get_entry(cache_key, vary_key);
    if (!entry.has_value())
        return {};

    if (associated_data_size > static_cast<u64>(NumericLimits<i64>::max()))
        return Error::from_string_literal("Associated data size exceeds the representable maximum");

    m_database->execute_statement(m_statements.update_associated_data_size, {}, static_cast<i64>(associated_data_size), encode_cache_key_for_database(cache_key), encode_cache_key_for_database(vary_key));

    adjust_total_estimated_size(-static_cast<i64>(entry->associated_data_size));
    adjust_total_estimated_size(static_cast<i64>(associated_data_size));
    entry->associated_data_size = associated_data_size;
    return {};
}

void CacheIndex::update_last_access_time(u64 cache_key, u64 vary_key)
{
    auto entry = get_entry(cache_key, vary_key);
    if (!entry.has_value())
        return;

    auto now = UnixDateTime::now();

    m_database->execute_statement(m_statements.update_last_access_time, {}, now, encode_cache_key_for_database(cache_key), encode_cache_key_for_database(vary_key));
    entry->last_access_time = now;
}

Optional<CacheIndex::Entry const&> CacheIndex::find_entry(u64 cache_key, HeaderList const& request_headers)
{
    auto& entries = m_entries.ensure(cache_key, [&]() {
        Vector<Entry> entries;

        m_database->execute_statement(
            m_statements.select_entries,
            [&](auto statement_id) -> ErrorOr<void> {
                int column = 0;

                auto vary_key = decode_cache_key_from_database(m_database->result_column<i64>(statement_id, column++));
                auto url = m_database->result_column<String>(statement_id, column++);
                auto request_headers = m_database->result_column<ByteString>(statement_id, column++);
                auto response_headers = m_database->result_column<ByteString>(statement_id, column++);
                auto data_size = m_database->result_column<i64>(statement_id, column++);
                auto associated_data_size = m_database->result_column<i64>(statement_id, column++);
                auto request_time = m_database->result_column<UnixDateTime>(statement_id, column++);
                auto response_time = m_database->result_column<UnixDateTime>(statement_id, column++);
                auto last_access_time = m_database->result_column<UnixDateTime>(statement_id, column++);

                if (data_size < 0 || associated_data_size < 0)
                    return {};

                entries.empend(vary_key, move(url), deserialize_headers(request_headers), deserialize_headers(response_headers), static_cast<u64>(data_size), static_cast<u64>(associated_data_size), request_headers.length(), response_headers.length(), request_time, response_time, last_access_time);
                return {};
            },
            encode_cache_key_for_database(cache_key));

        return entries;
    });

    return find_value(entries, [&](auto const& entry) {
        return create_vary_key(request_headers, entry.response_headers) == entry.vary_key;
    });
}

bool CacheIndex::has_entry(u64 cache_key, u64 vary_key)
{
    return get_entry(cache_key, vary_key).has_value();
}

Optional<CacheIndex::Entry&> CacheIndex::get_entry(u64 cache_key, u64 vary_key)
{
    auto entries = m_entries.get(cache_key);
    if (!entries.has_value())
        return {};

    return find_value(*entries, [&](auto const& entry) { return entry.vary_key == vary_key; });
}

void CacheIndex::adjust_total_estimated_size(i64 delta)
{
    Checked<i64> total = m_total_estimated_size;
    total += delta;
    if (total.has_overflow())
        m_total_estimated_size = delta > 0 ? NumericLimits<i64>::max() : 0;
    else
        m_total_estimated_size = max<i64>(total.value(), 0);
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
        [&](auto statement_id) -> ErrorOr<void> { sizes.since_requested_time = static_cast<u64>(m_database->result_column<i64>(statement_id, 0)); return {}; },
        since);

    m_database->execute_statement(
        m_statements.estimate_cache_size_accessed_since,
        [&](auto statement_id) -> ErrorOr<void> { sizes.total = static_cast<u64>(m_database->result_column<i64>(statement_id, 0)); return {}; },
        UnixDateTime::earliest());

    return sizes;
}

void CacheIndex::set_maximum_disk_cache_size(u64 maximum_disk_cache_size)
{
    auto new_maximum_disk_cache_size = compute_maximum_disk_cache_size(m_limits.free_disk_space, maximum_disk_cache_size);
    if (new_maximum_disk_cache_size == m_limits.maximum_disk_cache_size)
        return;

    m_limits.maximum_disk_cache_size = new_maximum_disk_cache_size;
    m_limits.maximum_disk_cache_entry_size = compute_maximum_disk_cache_entry_size(new_maximum_disk_cache_size);
}

}
