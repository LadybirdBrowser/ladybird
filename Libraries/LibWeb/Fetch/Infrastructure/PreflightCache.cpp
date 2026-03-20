/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/NeverDestroyed.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/PreflightCache.h>

namespace Web::Fetch::Infrastructure {

PreflightCache& PreflightCache::the()
{
    static NeverDestroyed<PreflightCache> s_cache;
    return *s_cache;
}

// https://fetch.spec.whatwg.org/#concept-cache-create-entry
void PreflightCache::create_a_new_cache_entry(Request const& request, AK::Duration max_age, Optional<ByteString> method, Optional<ByteString> header_name)
{
    // Already expired, no need to store.
    if (max_age.is_negative() || max_age.is_zero())
        return;

    auto partition_key = determine_the_network_partition_key(request);
    VERIFY(partition_key.has_value());

    // 1. Let entry be a cache entry, initialized as follows:
    Entry entry {
        .stored_at = MonotonicTime::now(),

        // key
        //    The result of determining the network partition key given request
        .key = partition_key.release_value(),

        // byte-serialized origin
        //     The result of byte-serializing a request origin with request
        .byte_serialized_origin = request.byte_serialize_origin(),

        // URL
        //    request’s current URL
        .url = request.current_url(),

        // max-age
        //     max-age
        .max_age = max_age,

        // credentials
        //     True if request’s credentials mode is "include", and false otherwise
        .credentials = request.credentials_mode() == Request::CredentialsMode::Include,

        // method
        //     method
        .method = move(method),

        // header name
        //     headerName
        .header_name = move(header_name),
    };

    // 2. Append entry to the user agent’s CORS-preflight cache.
    m_entries.append(move(entry));
}

// https://fetch.spec.whatwg.org/#concept-cache-match-method
bool PreflightCache::is_a_method_cache_entry_match(Request const& request, ByteString const& method)
{
    evict_expired_entries();

    // There is a method cache entry match for method using request when there is a cache entry in the user agent’s
    // CORS-preflight cache for which there is a cache entry match with request and its method is method or `*`.
    return method_cache_entry_match(request, method) != nullptr;
}

PreflightCache::Entry* PreflightCache::method_cache_entry_match(Request const& request, ByteString const& method)
{
    for (auto& entry : m_entries) {
        if (!entry.method.has_value() || (!entry.method->equals_ignoring_ascii_case(method) && entry.method != "*"sv))
            continue;
        if (is_cache_entry_match(entry, request))
            return &entry;
    }
    return nullptr;
}

void PreflightCache::cache_method(Request const& request, ByteString const& method, AK::Duration max_age)
{
    evict_expired_entries();

    if (auto* entry = method_cache_entry_match(request, method); entry != nullptr) {
        entry->update_max_age(max_age);
        return;
    }

    create_a_new_cache_entry(request, max_age, method, OptionalNone());
}

// https://fetch.spec.whatwg.org/#concept-cache-match-header
bool PreflightCache::is_a_header_name_cache_entry_match(Request const& request, ByteString const& header_name)
{
    evict_expired_entries();

    // There is a header-name cache entry match for headerName using request when there is a cache entry in the user
    // agent’s CORS-preflight cache for which there is a cache entry match with request and one of:
    //  * its header name is a byte-case-insensitive match for headerName
    //  * its header name is `*` and headerName is not a CORS non-wildcard request-header name
    // is true
    return header_name_cache_entry_match(request, header_name) != nullptr;
}

PreflightCache::Entry* PreflightCache::header_name_cache_entry_match(Request const& request, ByteString const& header_name)
{
    for (auto& entry : m_entries) {
        if (!entry.header_name.has_value())
            continue;
        if (!(entry.header_name->equals_ignoring_ascii_case(header_name) || (entry.header_name.value() == "*"sv && !is_cors_non_wildcard_request_header_name(header_name))))
            continue;
        if (is_cache_entry_match(entry, request))
            return &entry;
    }
    return nullptr;
}

void PreflightCache::cache_header_name(Request const& request, ByteString const& header_name, AK::Duration max_age)
{
    evict_expired_entries();

    if (auto* entry = header_name_cache_entry_match(request, header_name); entry != nullptr) {
        entry->update_max_age(max_age);
        return;
    }

    create_a_new_cache_entry(request, max_age, OptionalNone(), header_name);
}

// https://fetch.spec.whatwg.org/#concept-cache-match
bool PreflightCache::is_cache_entry_match(Entry const& entry, Request const& request)
{
    if (entry.has_expired())
        return false;

    // There is a cache entry match for a cache entry entry with request if
    // entry’s key is the result of determining the network partition key given request,
    if (entry.key != determine_the_network_partition_key(request))
        return false;

    // entry’s byte-serialized origin is the result of byte-serializing a request origin with request,
    if (entry.byte_serialized_origin != request.byte_serialize_origin())
        return false;

    // entry’s URL is request’s current URL,
    if (entry.url != request.current_url())
        return false;

    // and one of
    //  * entry’s credentials is true
    //  * entry’s credentials is false and request’s credentials mode is not "include".
    return entry.credentials || request.credentials_mode() != Request::CredentialsMode::Include;
}

void PreflightCache::evict_expired_entries()
{
    m_entries.remove_all_matching([](Entry const& entry) {
        return entry.has_expired();
    });
}

// https://fetch.spec.whatwg.org/#concept-cache-clear
void PreflightCache::clear_cache_entries(Request const& request)
{
    auto request_key = determine_the_network_partition_key(request);

    // To clear cache entries, given a request, remove any cache entries in the user agent’s CORS-preflight cache whose
    m_entries.remove_all_matching([&](Entry const& entry) {
        if (entry.has_expired())
            return true;

        // key is the result of determining the network partition key given request,
        if (entry.key != request_key)
            return false;

        // byte-serialized origin is the result of byte-serializing a request origin with request,
        if (entry.byte_serialized_origin != request.byte_serialize_origin())
            return false;

        // and URL is request’s current URL.
        if (entry.url != request.current_url())
            return false;

        return true;
    });
}

bool PreflightCache::Entry::has_expired() const
{
    return MonotonicTime::now() - stored_at >= max_age;
}

void PreflightCache::Entry::update_max_age(AK::Duration new_max_age)
{
    max_age = new_max_age;
    stored_at = MonotonicTime::now();
}

}
