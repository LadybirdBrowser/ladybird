/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibHTTP/Cache/Utilities.h>

namespace HTTP {

NonnullRefPtr<MemoryCache> MemoryCache::create()
{
    return adopt_ref(*new MemoryCache());
}

// https://httpwg.org/specs/rfc9111.html#constructing.responses.from.caches
Optional<MemoryCache::Entry const&> MemoryCache::open_entry(URL::URL const& url, StringView method, HeaderList const& request_headers) const
{
    // When presented with a request, a cache MUST NOT reuse a stored response unless:
    // - the presented target URI (Section 7.1 of [HTTP]) and that of the stored response match, and
    // - the request method associated with the stored response allows it to be used for the presented request, and
    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    auto cache_entry = m_complete_entries.get(cache_key);
    if (!cache_entry.has_value()) {
        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[31;1mHTTP CACHE MISS!\033[0m {}", url);
        return {};
    }

    // FIXME: - request header fields nominated by the stored response (if any) match those presented (see Section 4.1), and
    (void)request_headers;

    // FIXME: - the stored response does not contain the no-cache directive (Section 5.2.2.4), unless it is successfully validated (Section 4.3), and

    // FIXME: - the stored response is one of the following:
    //          + fresh (see Section 4.2), or
    //          + allowed to be served stale (see Section 4.2.4), or
    //          + successfully validated (see Section 4.3).

    dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[32;1mHTTP CACHE HIT!\033[0m {}", url);
    return cache_entry;
}

void MemoryCache::create_entry(URL::URL const& url, StringView method, u32 status_code, ByteString reason_phrase, HeaderList const& response_headers)
{
    if (!is_cacheable(method))
        return;
    if (!is_cacheable(status_code, response_headers))
        return;

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    auto response_headers_copy = HeaderList::create();
    store_header_and_trailer_fields(response_headers_copy, response_headers);

    Entry cache_entry {
        .status_code = status_code,
        .reason_phrase = move(reason_phrase),
        .response_headers = move(response_headers_copy),
        .response_body = {},
    };

    m_pending_entries.set(cache_key, move(cache_entry));
}

void MemoryCache::finalize_entry(URL::URL const& url, StringView method, ByteBuffer response_body)
{
    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    if (auto cache_entry = m_pending_entries.take(cache_key); cache_entry.has_value()) {
        cache_entry->response_body = move(response_body);
        m_complete_entries.set(cache_key, cache_entry.release_value());
    }
}

}
