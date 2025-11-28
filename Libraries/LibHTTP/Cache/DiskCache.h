/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
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
#include <LibURL/Forward.h>

namespace HTTP {

class DiskCache {
public:
    enum class Mode {
        Normal,

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
    Variant<Optional<CacheEntryReader&>, CacheHasOpenEntry> open_entry(CacheRequest&, URL::URL const&, StringView method, HeaderList const& request_headers);

    Requests::CacheSizes estimate_cache_size_accessed_since(UnixDateTime since);
    void remove_entries_accessed_since(UnixDateTime since);

    LexicalPath const& cache_directory() { return m_cache_directory; }

    void cache_entry_closed(Badge<CacheEntry>, CacheEntry const&);

private:
    DiskCache(Mode, NonnullRefPtr<Database::Database>, LexicalPath cache_directory, CacheIndex);

    enum class CheckReaderEntries {
        No,
        Yes,
    };
    bool check_if_cache_has_open_entry(CacheRequest&, u64 cache_key, URL::URL const&, CheckReaderEntries);

    Mode m_mode;

    NonnullRefPtr<Database::Database> m_database;

    HashMap<u64, Vector<NonnullOwnPtr<CacheEntry>, 1>> m_open_cache_entries;
    HashMap<u64, Vector<WeakPtr<CacheRequest>, 1>> m_requests_waiting_completion;

    LexicalPath m_cache_directory;
    CacheIndex m_index;
};

}
