/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibDatabase/Database.h>

namespace RequestServer {

// The cache index is a SQL database containing metadata about each cache entry. An entry in the index is created once
// the entire cache entry has been successfully written to disk.
class CacheIndex {
    struct Entry {
        u64 cache_key { 0 };

        String url;
        u64 data_size { 0 };

        UnixDateTime request_time;
        UnixDateTime response_time;
        UnixDateTime last_access_time;
    };

public:
    static ErrorOr<CacheIndex> create(Database::Database&);

    void create_entry(u64 cache_key, String url, u64 data_size, UnixDateTime request_time, UnixDateTime response_time);
    void remove_entry(u64 cache_key);
    void remove_all_entries();

    Optional<Entry&> find_entry(u64 cache_key);

    void update_last_access_time(u64 cache_key);

private:
    struct Statements {
        Database::StatementID insert_entry { 0 };
        Database::StatementID remove_entry { 0 };
        Database::StatementID remove_all_entries { 0 };
        Database::StatementID select_entry { 0 };
        Database::StatementID update_last_access_time { 0 };
    };

    CacheIndex(Database::Database&, Statements);

    Database::Database& m_database;
    Statements m_statements;

    HashMap<u32, Entry> m_entries;
};

}
