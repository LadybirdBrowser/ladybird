/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <RequestServer/Cache/CacheIndex.h>

namespace RequestServer {

ErrorOr<CacheIndex> CacheIndex::create(Database::Database& database)
{
    auto create_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS CacheIndex (
            cache_key INTEGER,
            url TEXT,
            data_size INTEGER,
            request_time INTEGER,
            response_time INTEGER,
            last_access_time INTEGER,
            PRIMARY KEY(cache_key)
        );)#"sv));
    database.execute_statement(create_table, {});

    Statements statements {};
    statements.insert_entry = TRY(database.prepare_statement("INSERT OR REPLACE INTO CacheIndex VALUES (?, ?, ?, ?, ?, ?);"sv));
    statements.remove_entry = TRY(database.prepare_statement("DELETE FROM CacheIndex WHERE cache_key = ?;"sv));
    statements.remove_all_entries = TRY(database.prepare_statement("DELETE FROM CacheIndex;"sv));
    statements.select_entry = TRY(database.prepare_statement("SELECT * FROM CacheIndex WHERE cache_key = ?;"sv));
    statements.update_last_access_time = TRY(database.prepare_statement("UPDATE CacheIndex SET last_access_time = ? WHERE cache_key = ?;"sv));

    return CacheIndex { database, statements };
}

CacheIndex::CacheIndex(Database::Database& database, Statements statements)
    : m_database(database)
    , m_statements(statements)
{
}

void CacheIndex::create_entry(u64 cache_key, String url, u64 data_size, UnixDateTime request_time, UnixDateTime response_time)
{
    auto now = UnixDateTime::now();

    Entry entry {
        .cache_key = cache_key,
        .url = move(url),
        .data_size = data_size,
        .request_time = request_time,
        .response_time = response_time,
        .last_access_time = now,
    };

    m_database.execute_statement(m_statements.insert_entry, {}, entry.cache_key, entry.url, entry.data_size, entry.request_time, entry.response_time, entry.last_access_time);
    m_entries.set(cache_key, move(entry));
}

void CacheIndex::remove_entry(u64 cache_key)
{
    m_database.execute_statement(m_statements.remove_entry, {}, cache_key);
    m_entries.remove(cache_key);
}

void CacheIndex::remove_all_entries()
{
    m_database.execute_statement(m_statements.remove_all_entries, {});
    m_entries.clear();
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
            auto data_size = m_database.result_column<u64>(statement_id, column++);
            auto request_time = m_database.result_column<UnixDateTime>(statement_id, column++);
            auto response_time = m_database.result_column<UnixDateTime>(statement_id, column++);
            auto last_access_time = m_database.result_column<UnixDateTime>(statement_id, column++);

            Entry entry { cache_key, move(url), data_size, request_time, response_time, last_access_time };
            m_entries.set(cache_key, move(entry));
        },
        cache_key);

    return m_entries.get(cache_key);
}

}
