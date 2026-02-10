/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibWeb/Fetch/Infrastructure/CORSPreflightCache.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>

namespace Web::Fetch::Infrastructure {

CORSPreflightCache& CORSPreflightCache::the()
{
    static CORSPreflightCache s_cache;
    return s_cache;
}

bool CORSPreflightCache::entry_matches_request(Entry const& entry, Request const& request) const
{
    // An entry matches a request if:
    // - Its network partition key matches the result of determining the network partition key given request.
    auto key = determine_the_network_partition_key(request);
    if (!key.has_value() || entry.network_partition_key != key.value())
        return false;

    // - Its origin is same origin as request's origin (when it is a URL::Origin).
    auto const* request_origin = request.origin().get_pointer<URL::Origin>();
    if (!request_origin || !entry.origin.is_same_origin(*request_origin))
        return false;

    // - Its URL equals request's current URL.
    if (entry.url != request.current_url())
        return false;

    // - Its credentials flag equals whether request's credentials mode is "include".
    if (entry.credentials != (request.credentials_mode() == Request::CredentialsMode::Include))
        return false;

    return true;
}

// https://fetch.spec.whatwg.org/#concept-cache-match
bool CORSPreflightCache::has_method_cache_entry_match(StringView method, Request const& request) const
{
    auto now = MonotonicTime::now();

    for (auto const& entry : m_entries) {
        // Skip header-name entries.
        if (!entry.method.has_value())
            continue;

        // Skip expired entries.
        if (now - entry.created_at > AK::Duration::from_seconds(entry.max_age))
            continue;

        if (!entry_matches_request(entry, request))
            continue;

        // The entry's method matches the given method, or the entry's method is `*` and
        // request's credentials mode is not "include".
        if (entry.method.value() == method)
            return true;
        if (entry.method.value() == "*"sv && request.credentials_mode() != Request::CredentialsMode::Include)
            return true;
    }

    return false;
}

// https://fetch.spec.whatwg.org/#concept-cache-match
bool CORSPreflightCache::has_header_name_cache_entry_match(StringView header_name, Request const& request) const
{
    auto now = MonotonicTime::now();

    for (auto const& entry : m_entries) {
        // Skip method entries.
        if (!entry.header_name.has_value())
            continue;

        // Skip expired entries.
        if (now - entry.created_at > AK::Duration::from_seconds(entry.max_age))
            continue;

        if (!entry_matches_request(entry, request))
            continue;

        // The entry's header name matches the given header name (byte-case-insensitive), or
        // the entry's header name is `*` and request's credentials mode is not "include" and
        // the given header name is not a CORS non-wildcard request-header name.
        if (entry.header_name.value().equals_ignoring_ascii_case(header_name))
            return true;
        if (entry.header_name.value() == "*"sv && request.credentials_mode() != Request::CredentialsMode::Include && !is_cors_non_wildcard_request_header_name(header_name))
            return true;
    }

    return false;
}

// https://fetch.spec.whatwg.org/#concept-cache-create
void CORSPreflightCache::create_entry(Request const& request, u64 max_age, Optional<ByteString> method, Optional<ByteString> header_name)
{
    auto key = determine_the_network_partition_key(request);
    if (!key.has_value())
        return;

    auto const* request_origin = request.origin().get_pointer<URL::Origin>();
    if (!request_origin)
        return;

    m_entries.append({
        .network_partition_key = key.release_value(),
        .origin = *request_origin,
        .url = request.current_url(),
        .max_age = max_age,
        .credentials = request.credentials_mode() == Request::CredentialsMode::Include,
        .method = move(method),
        .header_name = move(header_name),
        .created_at = MonotonicTime::now(),
    });
}

// https://fetch.spec.whatwg.org/#concept-cache-clear
void CORSPreflightCache::clear_entries(Request const& request)
{
    auto const* request_origin = request.origin().get_pointer<URL::Origin>();
    if (!request_origin)
        return;

    m_entries.remove_all_matching([&](Entry const& entry) {
        return entry.origin.is_same_origin(*request_origin) && entry.url == request.current_url();
    });
}

void CORSPreflightCache::clear_all()
{
    m_entries.clear();
}

}
