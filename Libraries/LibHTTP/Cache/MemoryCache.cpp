/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibHTTP/Cache/Utilities.h>

namespace HTTP {

NonnullRefPtr<MemoryCache> MemoryCache::create(NonnullRefPtr<MemoryCacheUsage> usage)
{
    return adopt_ref(*new MemoryCache(move(usage)));
}

MemoryCache::MemoryCache(NonnullRefPtr<MemoryCacheUsage> usage)
    : m_usage(move(usage))
{
}

MemoryCache::~MemoryCache()
{
    for (auto const& it : m_complete_entries) {
        for (auto const& entry : it.value)
            remove_from_usage(entry);
    }
}

// NB: A cache is only used on one thread, so a counter per thread orders the uses of all caches that share a usage.
static u64 next_use()
{
    static thread_local u64 s_last_use = 0;
    return ++s_last_use;
}

static size_t bytes_in_memory_of(MemoryCache::Entry const& entry)
{
    size_t size = sizeof(entry) + entry.reason_phrase.length();
    if (!entry.body_is_mapped)
        size += entry.response_body.size();
    for (auto const& header : *entry.response_headers)
        size += sizeof(header) + header.name.length() + header.value.length();
    return size;
}

void MemoryCache::add_to_usage(Entry const& entry)
{
    m_usage->bytes_in_memory += entry.bytes_in_memory;
    if (entry.body_is_mapped)
        ++m_usage->mapped_bodies;
}

void MemoryCache::remove_from_usage(Entry const& entry)
{
    VERIFY(m_usage->bytes_in_memory >= entry.bytes_in_memory);
    m_usage->bytes_in_memory -= entry.bytes_in_memory;
    if (entry.body_is_mapped) {
        VERIFY(m_usage->mapped_bodies > 0);
        --m_usage->mapped_bodies;
    }
}

// A stored response satisfies a request only if the request header fields nominated by the response's Vary header
// match those of the request that produced the entry, per RFC 9111 4.1.
static bool entry_matches_request(HeaderList const& request_headers, MemoryCache::Entry const& entry)
{
    return create_vary_key(request_headers, entry.response_headers) == entry.vary_key;
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
    Optional<Entry&> cache_entry;

    for (auto& entry : *cache_entries) {
        if (!entry_matches_request(request_headers, entry))
            continue;

        // https://httpwg.org/specs/rfc9111.html#caching.negotiated.responses
        // If multiple stored responses match, the cache will need to choose one to use. [...] If such a mechanism is
        // not available, or leads to equally preferred responses, the most recent response (as determined by the Date
        // header field) is chosen, as per Section 4.
        //
        // NB: Receipt time is available for every stored response.
        if (!cache_entry.has_value() || entry.response_time > cache_entry->response_time)
            cache_entry = entry;
    }

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
        cache_entry->last_use = next_use();
        return *cache_entry;

    case CacheLifetimeStatus::Expired:
    case CacheLifetimeStatus::MustRevalidate:
    case CacheLifetimeStatus::StaleWhileRevalidate:
        // NB: The "force-cache" and "only-if-cached" cache modes use a stored response "not paying attention to
        //     staleness", which includes responses that must be revalidated. Other engines behave the same way.
        if (cache_mode_permits_stale_responses(cache_mode)) {
            dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[32;1mOpened expired cache entry for\033[0m {} (lifetime={}s age={}s) ({} bytes)", url, freshness_lifetime.to_seconds(), current_age.to_seconds(), cache_entry->response_body.size());
            cache_entry->last_use = next_use();
            return *cache_entry;
        }

        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[33;1mCache entry expired for\033[0m {} (lifetime={}s age={}s)", url, freshness_lifetime.to_seconds(), current_age.to_seconds());
        remove_complete_entries(cache_key, [](auto const&) { return true; });
        return {};
    }

    VERIFY_NOT_REACHED();
}

void MemoryCache::create_entry(URL::URL const& url, StringView method, HeaderList const& request_headers, UnixDateTime request_time, u32 status_code, ByteString reason_phrase, HeaderList const& received_response_headers)
{
    auto response_headers_without_connection_specific_fields = remove_connection_specific_fields(received_response_headers);
    auto const& response_headers = *response_headers_without_connection_specific_fields;

    if (!is_cacheable(method, request_headers))
        return;

    // This 304 answers the client's preconditions, not the cache's validators.
    if (status_code == 304)
        return;

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    auto vary_key = create_vary_key(request_headers, response_headers);
    if (!vary_key.has_value() || !is_cacheable(status_code, response_headers)) {
        // A non-storable replacement must still retire the old response.
        remove_complete_entries(cache_key, [&](auto const& entry) { return entry_matches_request(request_headers, entry); });
        return;
    }

    auto response_headers_copy = HeaderList::create();
    store_header_and_trailer_fields(response_headers_copy, response_headers);

    Entry cache_entry {
        .vary_key = *vary_key,
        .status_code = status_code,
        .reason_phrase = move(reason_phrase),
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
void MemoryCache::finalize_entry(URL::URL const& url, StringView method, HeaderList const& request_headers, u32 status_code, HeaderList const& received_response_headers, Core::ImmutableBytes response_body)
{
    auto response_headers_without_connection_specific_fields = remove_connection_specific_fields(received_response_headers);
    auto const& response_headers = *response_headers_without_connection_specific_fields;

    if (!is_cacheable(method, request_headers))
        return;
    if (!is_cacheable(status_code, response_headers))
        return;

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);
    auto vary_key = create_vary_key(request_headers, response_headers);

    auto cache_entry = take_pending_entry(cache_key, vary_key);
    if (!cache_entry.has_value())
        return;

    dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[34;1mFinished caching\033[0m {} ({} bytes)", url, response_body.size());
    cache_entry->body_is_mapped = response_body.is_file_backed() || response_body.is_readonly_mapped();
    cache_entry->response_body = move(response_body);
    // NB: The response headers are shared with the responses made from this entry, so this is counted once.
    cache_entry->bytes_in_memory = bytes_in_memory_of(*cache_entry);
    cache_entry->last_use = next_use();

    remove_complete_entries(cache_key, [&](auto const& entry) { return entry.vary_key == cache_entry->vary_key; });
    add_to_usage(*cache_entry);
    m_complete_entries.ensure(cache_key).append(cache_entry.release_value());
}

void MemoryCache::abandon_entry(URL::URL const& url, StringView method, HeaderList const& request_headers, HeaderList const& received_response_headers, StoredResponse stored_response)
{
    auto response_headers_without_connection_specific_fields = remove_connection_specific_fields(received_response_headers);
    auto const& response_headers = *response_headers_without_connection_specific_fields;

    if (!is_cacheable(method, request_headers))
        return;

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);
    auto vary_key = create_vary_key(request_headers, response_headers);

    if (take_pending_entry(cache_key, vary_key).has_value())
        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[33;1mAbandoned cache entry for\033[0m {}", url);

    if (stored_response == StoredResponse::Retire)
        remove_complete_entries(cache_key, [&](auto const& entry) { return entry_matches_request(request_headers, entry); });
}

Optional<MemoryCache::Entry> MemoryCache::take_pending_entry(u64 cache_key, Optional<u64> vary_key)
{
    auto cache_entries = m_pending_entries.get(cache_key);
    if (!cache_entries.has_value())
        return {};

    auto index = cache_entries->find_first_index_if([&](auto const& entry) {
        return vary_key == entry.vary_key;
    });
    if (!index.has_value())
        return {};

    auto cache_entry = cache_entries->take(*index);
    if (cache_entries->is_empty())
        m_pending_entries.remove(cache_key);
    return cache_entry;
}

// https://httpwg.org/specs/rfc9111.html#invalidation
void MemoryCache::invalidate(URL::URL const& url)
{
    // "Invalidate" means that the cache will either remove all stored responses whose target URI matches the given URI
    // or mark them as "invalid" and in need of a mandatory validation before they can be sent in response to a
    // subsequent request.
    auto serialized_url = serialize_url_for_cache_storage(url);

    for (auto method : { "GET"sv, "HEAD"sv }) {
        auto cache_key = create_cache_key(serialized_url, method);

        dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[33;1mInvalidated cache entries for\033[0m {} {}", method, url);
        remove_complete_entries(cache_key, [](auto const&) { return true; });
        m_pending_entries.remove(cache_key);
    }
}

void MemoryCache::remove_complete_entries(u64 cache_key, Function<bool(Entry const&)> const& predicate)
{
    auto cache_entries = m_complete_entries.get(cache_key);
    if (!cache_entries.has_value())
        return;

    cache_entries->remove_all_matching([&](auto const& entry) {
        if (!predicate(entry))
            return false;
        remove_from_usage(entry);
        return true;
    });
    if (cache_entries->is_empty())
        m_complete_entries.remove(cache_key);
}

Vector<MemoryCache::EntryUse> MemoryCache::complete_entry_uses() const
{
    Vector<EntryUse> uses;
    for (auto const& [cache_key, cache_entries] : m_complete_entries) {
        for (auto const& entry : cache_entries)
            uses.append({ .cache_key = cache_key, .vary_key = entry.vary_key, .body_is_mapped = entry.body_is_mapped, .last_use = entry.last_use });
    }
    return uses;
}

void MemoryCache::evict_complete_entry(u64 cache_key, u64 vary_key)
{
    dbgln_if(HTTP_MEMORY_CACHE_DEBUG, "\033[37m[memory]\033[0m \033[33;1mEvicted cache entry\033[0m {}", cache_key);
    remove_complete_entries(cache_key, [&](auto const& entry) { return entry.vary_key == vary_key; });
}

}
