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
#include <LibDatabase/Database.h>
#include <LibURL/Forward.h>
#include <RequestServer/Cache/CacheEntry.h>
#include <RequestServer/Cache/CacheIndex.h>

namespace RequestServer {

class DiskCache {
public:
    static ErrorOr<DiskCache> create();

    struct CacheHasOpenEntry { };
    Variant<Optional<CacheEntryWriter&>, CacheHasOpenEntry> create_entry(Request&);
    Variant<Optional<CacheEntryReader&>, CacheHasOpenEntry> open_entry(Request&);

    Requests::CacheSizes estimate_cache_size_accessed_since(UnixDateTime since) const;
    void remove_entries_accessed_since(UnixDateTime since);

    LexicalPath const& cache_directory() { return m_cache_directory; }

    void cache_entry_closed(Badge<CacheEntry>, CacheEntry const&);

private:
    DiskCache(NonnullRefPtr<Database::Database>, LexicalPath cache_directory, CacheIndex);

    enum class CheckReaderEntries {
        No,
        Yes,
    };
    bool check_if_cache_has_open_entry(Request&, u64 cache_key, CheckReaderEntries);

    NonnullRefPtr<Database::Database> m_database;

    HashMap<u64, Vector<NonnullOwnPtr<CacheEntry>, 1>> m_open_cache_entries;
    HashMap<u64, Vector<WeakPtr<Request>, 1>> m_requests_waiting_completion;

    LexicalPath m_cache_directory;
    CacheIndex m_index;
};

}
