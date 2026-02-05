/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/CacheSizes.h>

namespace HTTP {

// The cache index is a SQL database containing metadata about each cache entry. An entry in the index is created once
// the entire cache entry has been successfully written to disk.
class CacheIndex {
    struct Entry {
        u64 vary_key { 0 };

        String url;
        NonnullRefPtr<HeaderList> request_headers;
        NonnullRefPtr<HeaderList> response_headers;
        u64 data_size { 0 };

        UnixDateTime request_time;
        UnixDateTime response_time;
        UnixDateTime last_access_time;
    };

public:
    static ErrorOr<CacheIndex> create(Database::Database&);

    ErrorOr<void> create_entry(u64 cache_key, u64 vary_key, String url, NonnullRefPtr<HeaderList> request_headers, NonnullRefPtr<HeaderList> response_headers, u64 data_size, UnixDateTime request_time, UnixDateTime response_time);
    void remove_entry(u64 cache_key, u64 vary_key);
    void remove_entries_exceeding_cache_limit(Function<void(u64 cache_key, u64 vary_key)> on_entry_removed);
    void remove_entries_accessed_since(UnixDateTime, Function<void(u64 cache_key, u64 vary_key)> on_entry_removed);

    Optional<Entry const&> find_entry(u64 cache_key, HeaderList const& request_headers);

    void update_response_headers(u64 cache_key, u64 vary_key, NonnullRefPtr<HeaderList>);
    void update_last_access_time(u64 cache_key, u64 vary_key);

    Requests::CacheSizes estimate_cache_size_accessed_since(UnixDateTime since);

    void set_maximum_disk_cache_size(u64 maximum_disk_cache_size);

private:
    struct Statements {
        Database::StatementID insert_entry { 0 };
        Database::StatementID remove_entry { 0 };
        Database::StatementID remove_entries_exceeding_cache_limit { 0 };
        Database::StatementID remove_entries_accessed_since { 0 };
        Database::StatementID select_entries { 0 };
        Database::StatementID update_response_headers { 0 };
        Database::StatementID update_last_access_time { 0 };
        Database::StatementID estimate_cache_size_accessed_since { 0 };
    };

    struct Limits {
        u64 free_disk_space { 0 };
        u64 maximum_disk_cache_size { 0 };
        u64 maximum_disk_cache_entry_size { 0 };
    };

    CacheIndex(Database::Database&, Statements, Limits);

    Optional<Entry&> get_entry(u64 cache_key, u64 vary_key);
    void delete_entry(u64 cache_key, u64 vary_key);

    NonnullRawPtr<Database::Database> m_database;
    Statements m_statements;

    HashMap<u64, Vector<Entry>> m_entries;

    Limits m_limits;
};

}
