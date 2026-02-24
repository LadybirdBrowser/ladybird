/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/WeakPtr.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/Cache/CacheEntry.h>
#include <LibHTTP/Cache/CacheIndex.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibURL/Forward.h>

namespace HTTP {

class DiskCache {
public:
    enum class Mode {
        Normal,

        // In partitioned mode, the cache is enabled as normal, but each RequestServer process operates with a unique
        // disk cache database.
        Partitioned,

        // In test mode, we only enable caching of responses on a per-request basis, signified by a request header. The
        // response headers will include some status on how the request was handled.
        Testing,
    };
    static ErrorOr<DiskCache> create(Mode);

    DiskCache(DiskCache&&);
    DiskCache& operator=(DiskCache&&);

    ~DiskCache();

    Mode mode() const { return m_mode; }

    struct CacheHasOpenEntry { };
    Variant<Optional<CacheEntryWriter&>, CacheHasOpenEntry> create_entry(CacheRequest&, URL::URL const&, StringView method, HeaderList const& request_headers, UnixDateTime request_start_time);

    enum class OpenMode {
        Read,
        Revalidate,
    };
    Variant<Optional<CacheEntryReader&>, CacheHasOpenEntry> open_entry(CacheRequest&, URL::URL const&, StringView method, HeaderList const& request_headers, CacheMode, OpenMode);

    void remove_entries_exceeding_cache_limit();
    void set_maximum_disk_cache_size(u64 maximum_disk_cache_size);

    Requests::CacheSizes estimate_cache_size_accessed_since(UnixDateTime since);
    void remove_entries_accessed_since(UnixDateTime since);

    LexicalPath const& cache_directory() const { return m_cache_directory; }

    void cache_entry_closed(Badge<CacheEntry>, CacheEntry const&);

private:
    DiskCache(Mode, NonnullRefPtr<Database::Database>, LexicalPath cache_directory, CacheIndex);

    enum class CheckReaderEntries {
        No,
        Yes,
    };
    bool check_if_cache_has_open_entry(CacheRequest&, u64 cache_key, URL::URL const&, CheckReaderEntries);

    void delete_entry(u64 cache_key, u64 vary_key);

    Mode m_mode;
    Optional<String> m_partitioned_cache_key;

    NonnullRefPtr<Database::Database> m_database;

    struct OpenCacheEntry {
        NonnullOwnPtr<CacheEntry> entry;
        WeakPtr<CacheRequest> request;
    };
    HashMap<u64, Vector<OpenCacheEntry, 1>, IdentityHashTraits<u64>> m_open_cache_entries;
    HashMap<u64, Vector<WeakPtr<CacheRequest>, 1>, IdentityHashTraits<u64>> m_requests_waiting_completion;

    LexicalPath m_cache_directory;
    CacheIndex m_index;
};

}
