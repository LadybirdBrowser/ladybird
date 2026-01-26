/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
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
Optional<MemoryCache::Entry const&> MemoryCache::open_entry(URL::URL const& url, StringView method, HeaderList const& request_headers, CacheMode cache_mode)
{
    if (cache_mode == CacheMode::Reload || cache_mode == CacheMode::NoCache)
        return {};

    // When presented with a request, a cache MUST NOT reuse a stored response unless:
    // - the presented target URI (Section 7.1 of [HTTP]) and that of the stored response match, and
    // - the request method associated with the stored response allows it to be used for the presented request, and
    if (!is_cacheable(method, request_headers))
        return {};

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    auto cache_entries = m_complete_entries.get(cache_key);
    if (!cache_entries.has_value()) {
        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[35;1mNo cache entry for\033[0m {}", url);
        return {};
    }

    // - request header fields nominated by the stored response (if any) match those presented (see Section 4.1), and
    auto cache_entry = find_value(*cache_entries, [&](auto const& entry) {
        return create_vary_key(request_headers, entry.response_headers) == entry.vary_key;
    });
    if (!cache_entry.has_value()) {
        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[35;1mVary mismatch for\033[0m {}", url);
        return {};
    }

    // - the stored response does not contain the no-cache directive (Section 5.2.2.4), unless it is successfully
    //   validated (Section 4.3), and
    // - the stored response is one of the following:
    //       * fresh (see Section 4.2), or
    //       * allowed to be served stale (see Section 4.2.4), or
    //       * successfully validated (see Section 4.3).
    auto freshness_lifetime = calculate_freshness_lifetime(cache_entry->status_code, cache_entry->response_headers);
    auto current_age = calculate_age(cache_entry->response_headers, cache_entry->request_time, cache_entry->response_time);

    switch (cache_lifetime_status(request_headers, cache_entry->response_headers, freshness_lifetime, current_age)) {
    case CacheLifetimeStatus::Fresh:
        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[32;1mOpened cache entry for\033[0m {} (lifetime={}s age={}s) ({} bytes)", url, freshness_lifetime.to_seconds(), current_age.to_seconds(), cache_entry->response_body.size());
        return cache_entry;

    case CacheLifetimeStatus::Expired:
    case CacheLifetimeStatus::MustRevalidate:
    case CacheLifetimeStatus::StaleWhileRevalidate:
        if (cache_mode_permits_stale_responses(cache_mode)) {
            dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[32;1mOpened expired cache entry for\033[0m {} (lifetime={}s age={}s) ({} bytes)", url, freshness_lifetime.to_seconds(), current_age.to_seconds(), cache_entry->response_body.size());
            return cache_entry;
        }

        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[33;1mCache entry expired for\033[0m {} (lifetime={}s age={}s)", url, freshness_lifetime.to_seconds(), current_age.to_seconds());
        m_complete_entries.remove(cache_key);
        return {};
    }

    VERIFY_NOT_REACHED();
}

void MemoryCache::create_entry(URL::URL const& url, StringView method, HeaderList const& request_headers, UnixDateTime request_time, u32 status_code, ByteString reason_phrase, HeaderList const& response_headers)
{
    if (!is_cacheable(method, request_headers))
        return;
    if (!is_cacheable(status_code, response_headers))
        return;

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);
    auto vary_key = create_vary_key(request_headers, response_headers);

    auto request_headers_copy = HeaderList::create();
    store_header_and_trailer_fields(request_headers_copy, request_headers);

    auto response_headers_copy = HeaderList::create();
    store_header_and_trailer_fields(response_headers_copy, response_headers);

    Entry cache_entry {
        .vary_key = vary_key,
        .status_code = status_code,
        .reason_phrase = move(reason_phrase),
        .request_headers = move(request_headers_copy),
        .response_headers = move(response_headers_copy),
        .response_body = {},
        .request_time = request_time,
        .response_time = UnixDateTime::now(),
    };

    dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[32;1mCreated cache entry for\033[0m {}", url);
    m_pending_entries.ensure(cache_key).append(move(cache_entry));
}

// FIXME: It would be nicer if create_entry just returned the cache and vary keys. But the call sites of create_entry and
//        finalize_entry are pretty far apart, so passing that information along is rather awkward in Fetch.
void MemoryCache::finalize_entry(URL::URL const& url, StringView method, HeaderList const& request_headers, u32 status_code, HeaderList const& response_headers, ByteBuffer response_body)
{
    if (!is_cacheable(method, request_headers))
        return;
    if (!is_cacheable(status_code, response_headers))
        return;

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);
    auto vary_key = create_vary_key(request_headers, response_headers);

    if (auto cache_entries = m_pending_entries.get(cache_key); cache_entries.has_value()) {
        auto index = cache_entries->find_first_index_if([&](auto const& entry) {
            return vary_key == entry.vary_key;
        });
        if (!index.has_value())
            return;

        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[34;1mFinished caching\033[0m {} ({} bytes)", url, response_body.size());

        auto cache_entry = cache_entries->take(*index);
        cache_entry.response_body = move(response_body);

        if (cache_entries->is_empty())
            m_pending_entries.remove(cache_key);

        m_complete_entries.ensure(cache_key).append(move(cache_entry));
    }
}

}
