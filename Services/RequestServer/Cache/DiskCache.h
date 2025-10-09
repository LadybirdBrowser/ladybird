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
#include <LibHTTP/HeaderMap.h>
#include <LibURL/Forward.h>
#include <RequestServer/Cache/CacheEntry.h>
#include <RequestServer/Cache/CacheIndex.h>

namespace RequestServer {

class DiskCache {
public:
    static ErrorOr<DiskCache> create();

    Optional<CacheEntryWriter&> create_entry(URL::URL const&, StringView method, u32 status_code, Optional<String> reason_phrase, HTTP::HeaderMap const&, UnixDateTime request_time);
    Optional<CacheEntryReader&> open_entry(URL::URL const&, StringView method);
    void clear_cache();

    LexicalPath const& cache_directory() { return m_cache_directory; }

    void cache_entry_closed(Badge<CacheEntry>, CacheEntry const&);

private:
    DiskCache(NonnullRefPtr<Database::Database>, LexicalPath cache_directory, CacheIndex);

    NonnullRefPtr<Database::Database> m_database;

    HashMap<FlatPtr, NonnullOwnPtr<CacheEntry>> m_open_cache_entries;

    LexicalPath m_cache_directory;
    CacheIndex m_index;
};

}
